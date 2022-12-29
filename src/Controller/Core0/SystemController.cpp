//
// Created by Magnus Nordlander on 2021-07-06.
//

#include <utils/hex_format.h>
#include "SystemController.h"
#include "pico/timeout_helper.h"
#include <cmath>
#include <hardware/timer.h>
#include "Utils/UartReadBlockingTimeout.h"
#include "Utils/ClearUartCruft.h"

SystemController::SystemController(
        uart_inst_t * _uart,
        PicoQueue<SystemControllerStatusMessage> *outgoingQueue,
        PicoQueue<SystemControllerCommand> *incomingQueue,
        SystemSettings *settings)
        :
        outgoingQueue(outgoingQueue),
        incomingQueue(incomingQueue),
        uart(_uart),
        settings(settings),
        brewBoilerController(settings->getTargetBrewTemp(), 20.0f, settings->getBrewPidParameters(), 2.0f),
        serviceBoilerController(settings->getTargetServiceTemp(), 0.5f){
    safeLccRawPacket = create_safe_packet();
    currentLccParsedPacket = LccParsedPacket();
}

void SystemController::sendSafePacketNoWait() {
    uart_clear_cruft(uart);
    uart_write_blocking(uart, (uint8_t *) &safeLccRawPacket, sizeof(safeLccRawPacket));
}

void SystemController::loop() {
    if (internalState == NOT_STARTED_YET) {
        auto timeout = make_timeout_time_ms(100);

        sendSafePacketNoWait();

        sleep_until(timeout);

        handleCommands();

        return;
    }

    uart_clear_cruft(uart);
    sendLccPacket();

    // This timeout is used both as a timeout for reading from the UART and to know when to send the next packet.
    auto timeout = make_timeout_time_ms(100);

    bool success = uart_read_blocking_timeout(uart, reinterpret_cast<uint8_t *>(&currentControlBoardRawPacket),
                                              sizeof(currentControlBoardRawPacket), timeout);

    if (!success) {
        softBail(BAIL_REASON_CB_UNRESPONSIVE);
    }

    uint16_t cbValidation = validate_raw_packet(currentControlBoardRawPacket);

    if (cbValidation) {
        softBail(BAIL_REASON_CB_PACKET_INVALID);
    }

    if (isBailed()) {
        if (isSoftBailed()) {
            if (!success) {
                unbailTimer.reset();
            } else if (!unbailTimer.has_value()) {
                unbailTimer = get_absolute_time();
            } else if (absolute_time_diff_us(unbailTimer.value(), get_absolute_time()) > 2000000) {
                unbail();
            }
        }
    } else if (internalState == RUNNING) {
        currentControlBoardParsedPacket = convert_raw_control_board_packet(currentControlBoardRawPacket);

        if (runState == RUN_STATE_UNDETEMINED) {
            if (currentControlBoardParsedPacket.brew_boiler_temperature < 65) {
                initiateHeatup();
            } else {
                runState = RUN_STATE_NORMAL;
            }
        } else if (runState == RUN_STATE_HEATUP_STAGE_1) {
            if (currentControlBoardParsedPacket.brew_boiler_temperature > 128) {
                transitionToHeatupStage2();
            }
        } else if (runState == RUN_STATE_HEATUP_STAGE_2) {
            if (absolute_time_diff_us(heatupStage2Timer.value(), get_absolute_time()) > 4 * 60 * 1000 * 1000) {
                finishHeatup();
            }
        }
    }

    // Reset the current raw packet.
    currentControlBoardRawPacket = ControlBoardRawPacket();

    handleCommands();

    if (!isBailed()) {
        currentLccParsedPacket = handleControlBoardPacket(currentControlBoardParsedPacket);
    } else {
        currentLccParsedPacket = convert_lcc_raw_to_parsed(safeLccRawPacket);
    }

    SystemControllerStatusMessage message = {
            .timestamp = get_absolute_time(),
            .brewTemperature = static_cast<float>(brewTempAverage.average()),
            .brewSetPoint = settings->getTargetBrewTemp(),
            .brewPidSettings = settings->getBrewPidParameters(),
            .brewPidParameters = brewPidRuntimeParameters,
            .serviceTemperature = static_cast<float>(serviceTempAverage.average()),
            .serviceSetPoint = settings->getTargetServiceTemp(),
            .servicePidSettings = settings->getServicePidParameters(),
            .servicePidParameters = servicePidRuntimeParameters,
            .brewSSRActive = currentLccParsedPacket.brew_boiler_ssr_on,
            .serviceSSRActive = currentLccParsedPacket.service_boiler_ssr_on,
            .ecoMode = settings->getEcoMode(),
            .state = externalState(),
            .bailReason = bail_reason,
            .currentlyBrewing = currentControlBoardParsedPacket.brew_switch && currentLccParsedPacket.pump_on,
            .currentlyFillingServiceBoiler = currentLccParsedPacket.pump_on &&
                                             currentLccParsedPacket.service_boiler_solenoid_open,
            .waterTankLow = currentControlBoardParsedPacket.water_tank_empty,
            .lastSleepModeExitAt = lastSleepModeExitAt
    };

    if (!outgoingQueue->isFull()) {
        outgoingQueue->tryAdd(&message);
    }

    settings->writeSettingsIfChanged();

    sleep_until(timeout);
}

void SystemController::sendLccPacket() {
    LccRawPacket rawLccPacket = convert_lcc_parsed_to_raw(currentLccParsedPacket);

    uint16_t lccValidation = validate_lcc_raw_packet(rawLccPacket);
    if (lccValidation) {
        hardBail(BAIL_REASON_LCC_PACKET_INVALID);
    }

    if (onlySendSafePackages()) {
        uart_write_blocking(uart, (uint8_t *) &safeLccRawPacket, sizeof(safeLccRawPacket));
    } else {
        uart_write_blocking(uart, (uint8_t *) &rawLccPacket, sizeof(rawLccPacket));
    }
}

LccParsedPacket SystemController::handleControlBoardPacket(ControlBoardParsedPacket latestParsedPacket) {
    LccParsedPacket lcc;

    waterTankEmptyLatch.set(latestParsedPacket.water_tank_empty);
    serviceBoilerLowLatch.set(latestParsedPacket.service_boiler_low);

    brewTempAverage.addValue(latestParsedPacket.brew_boiler_temperature);
    serviceTempAverage.addValue(latestParsedPacket.service_boiler_temperature);

    bool brewing = false;

    // If we're not already brewing, don't start a brew or fill the service boiler if there is no water in the tank
    if (!brewStartedAt.has_value()) {
        if (!waterTankEmptyLatch.get()) {
            if (latestParsedPacket.brew_switch) {
                lcc.pump_on = true;
                brewing = true;
                brewStartedAt = get_absolute_time();
            } else if (serviceBoilerLowLatch.get()) { // Starting a brew has priority over filling the service boiler
                lcc.pump_on = true;
                lcc.service_boiler_solenoid_open = true;
            }
        }
    } else { // If we are brewing, keep brewing even if there is no water in the tank
        if (latestParsedPacket.brew_switch) {
            lcc.pump_on = true;
            brewing = true;
        } else { // Filling the service boiler is not an option while brewing
            brewStartedAt.reset();
        }
    }

    /*
     * New algorithm:
     *
     * Cap BB and SB at 25 respectively. Divide time into 25 x 100 ms slots.
     *
     * If BB + SB < 25: Both get what they want.
     * Else: They get a proportional share of what they want.
     *   I.e. if BB = 17 and SB = 13, BB gets round((17/(17+13))*25) = 14 and SB gets round((13/(17+13))*25) = 11.
     */
    if (ssrStateQueue.isEmpty()) {
        float feedForward = 0.0f;
        if (brewStartedAt.has_value()) {
            feedForward = feedForwardK * ((float)absolute_time_diff_us(brewStartedAt.value(), get_absolute_time()) / 1000.f) + feedForwardM;
        }

        uint8_t bbSignal = brewBoilerController.getControlSignal(
                brewTempAverage.average(),
                brewing ? feedForward : 0.f,
                shouldForceHysteresisForBrewBoiler()
                );
        uint8_t sbSignal = serviceBoilerController.getControlSignal(serviceTempAverage.average());

//        printf("Raw signals. BB: %u SB: %u\n", bbSignal, sbSignal);

        if (settings->getEcoMode()) {
            sbSignal = 0;
        }

        // Power-sharing
        if (bbSignal + sbSignal > 25) {
            if (!brewing) {
                // If we're brewing, prioritize the brew boiler fully
                // Otherwise, give the brew boiler slightly less than 75% priority
                bbSignal = floor((float)bbSignal * 0.75);
            }
            sbSignal = 25 - bbSignal;
        }

        uint8_t noSignal = 25 - bbSignal - sbSignal;

        //printf("Adding new controls to the queue. BB: %u SB: %u NB: %u\n", bbSignal, sbSignal, noSignal);

        for (uint8_t i = 0; i < bbSignal; ++i) {
            SsrState state = BREW_BOILER_SSR_ON;
            ssrStateQueue.tryAdd(&state);
        }

        for (uint8_t i = 0; i < sbSignal; ++i) {
            SsrState state = SERVICE_BOILER_SSR_ON;
            ssrStateQueue.tryAdd(&state);
        }

        for (uint8_t i = 0; i < noSignal; ++i) {
            SsrState state = BOTH_SSRS_OFF;
            ssrStateQueue.tryAdd(&state);
        }
    }

    SsrState state;
    if (!ssrStateQueue.tryRemove(&state)) {
        hardBail(BAIL_REASON_SSR_QUEUE_EMPTY);
        return lcc;
    }

    if (state == BREW_BOILER_SSR_ON) {
        lcc.brew_boiler_ssr_on = true;
    } else if (state == SERVICE_BOILER_SSR_ON) {
        lcc.service_boiler_ssr_on = true;
    }

    brewPidRuntimeParameters = brewBoilerController.getRuntimeParameters();
    //servicePidRuntimeParameters = serviceBoilerController.getRuntimeParameters();

    return lcc;
}

void SystemController::handleCommands() {
    SystemControllerCommand command;
    //printf("Q: %u\n", incomingQueue->getLevelUnsafe());
    while (!incomingQueue->isEmpty()) {
        incomingQueue->removeBlocking(&command);

        switch (command.type) {
            case COMMAND_SET_BREW_SET_POINT:
                settings->setTargetBrewTemp(command.float1);
                break;
            case COMMAND_SET_BREW_PID_PARAMETERS:
                settings->setBrewPidParameters(PidSettings{
                    .Kp = command.float1,
                    .Ki = command.float2,
                    .Kd = command.float3,
                    .windupLow = command.float4,
                    .windupHigh = command.float5
                });
                break;
            case COMMAND_SET_SERVICE_SET_POINT:
                settings->setTargetServiceTemp(command.float1);
                break;
            case COMMAND_SET_SERVICE_PID_PARAMETERS:
                settings->setServicePidParameters(PidSettings{
                        .Kp = command.float1,
                        .Ki = command.float2,
                        .Kd = command.float3,
                        .windupLow = command.float4,
                        .windupHigh = command.float5
                });
                break;
            case COMMAND_SET_ECO_MODE:
                settings->setEcoMode(command.bool1);
                break;
            case COMMAND_SET_SLEEP_MODE:
                setSleepMode(command.bool1);
                break;
            case COMMAND_UNBAIL:
                unbail();
                break;
            case COMMAND_TRIGGER_FIRST_RUN:
                break;
            case COMMAND_BEGIN:
                internalState = RUNNING;
                break;
        }
    }

    updateControllerSettings();
}

void SystemController::updateControllerSettings() {
    brewBoilerController.setPidParameters(settings->getBrewPidParameters());
    //serviceBoilerController.setPidParameters(servicePidParameters);

    if (settings->getSleepMode()) {
        brewBoilerController.updateSetPoint(70.f);
        serviceBoilerController.updateSetPoint(70.f);
    } else if (runState == RUN_STATE_HEATUP_STAGE_1) {
        brewBoilerController.updateSetPoint(130.f);
        serviceBoilerController.updateSetPoint(0.f);
    } else if (runState == RUN_STATE_HEATUP_STAGE_2) {
        brewBoilerController.updateSetPoint(130.f);
        serviceBoilerController.updateSetPoint(settings->getTargetServiceTemp());
    } else {
        brewBoilerController.updateSetPoint(settings->getTargetBrewTemp());
        serviceBoilerController.updateSetPoint(settings->getTargetServiceTemp());
    }
}

void SystemController::softBail(SystemControllerBailReason reason) {
    if (internalState != HARD_BAIL) {
        internalState = SOFT_BAIL;
    }

    if (bail_reason == BAIL_REASON_NONE) {
        bail_reason = reason;
    }
}

void SystemController::hardBail(SystemControllerBailReason reason) {
    internalState = HARD_BAIL;
    bail_reason = reason;
}

SystemControllerState SystemController::externalState() {
    switch (internalState) {
        case NOT_STARTED_YET:
            return SYSTEM_CONTROLLER_STATE_UNDETERMINED;
        case RUNNING:
            if (settings->getSleepMode()) {
                return SYSTEM_CONTROLLER_STATE_SLEEPING;
            }

            switch (runState) {
                case RUN_STATE_UNDETEMINED:
                    return SYSTEM_CONTROLLER_STATE_UNDETERMINED;
                case RUN_STATE_HEATUP_STAGE_1:
                case RUN_STATE_HEATUP_STAGE_2:
                    return SYSTEM_CONTROLLER_STATE_HEATUP;
                case RUN_STATE_NORMAL:
                    return areTemperaturesAtSetPoint() ? SYSTEM_CONTROLLER_STATE_WARM : SYSTEM_CONTROLLER_STATE_TEMPS_NORMALIZING;
            }
        case SOFT_BAIL:
        case HARD_BAIL:
            return SYSTEM_CONTROLLER_STATE_BAILED;
    }

    return SYSTEM_CONTROLLER_STATE_UNDETERMINED;
}

void SystemController::unbail() {
    internalState = RUNNING;
    runState = RUN_STATE_UNDETEMINED;
    bail_reason = BAIL_REASON_NONE;
    unbailTimer.reset();
}

void SystemController::setSleepMode(bool _sleepMode) {
    if (_sleepMode && runState == RUN_STATE_HEATUP_STAGE_2) {
        heatupStage2Timer.reset();
    }

    if (!_sleepMode) {
        lastSleepModeExitAt = get_absolute_time();
    }

    settings->setSleepMode(_sleepMode);

    updateControllerSettings();
}

void SystemController::initiateHeatup() {
    runState = RUN_STATE_HEATUP_STAGE_1;
    updateControllerSettings();
}

void SystemController::transitionToHeatupStage2() {
    runState = RUN_STATE_HEATUP_STAGE_2;
    heatupStage2Timer = get_absolute_time();
    updateControllerSettings();
}

void SystemController::finishHeatup() {
    runState = RUN_STATE_NORMAL;
    heatupStage2Timer.reset();
    updateControllerSettings();
}

bool SystemController::areTemperaturesAtSetPoint() const {
    float bbsplo = settings->getTargetBrewTemp() - 2.f;
    float bbsphi = settings->getTargetBrewTemp() + 2.f;
    float sbsplo = settings->getTargetServiceTemp() - 4.f;
    float sbsphi = settings->getTargetServiceTemp() + 4.f;

    if (currentControlBoardParsedPacket.brew_boiler_temperature < bbsplo || currentControlBoardParsedPacket.brew_boiler_temperature > bbsphi) {
        return false;
    }

    if (!settings->getEcoMode() && (currentControlBoardParsedPacket.service_boiler_temperature < sbsplo || currentControlBoardParsedPacket.service_boiler_temperature > sbsphi)) {
        return false;
    }

    return true;
}