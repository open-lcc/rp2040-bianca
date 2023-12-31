//
// Created by Magnus Nordlander on 2021-07-06.
//

#ifndef FIRMWARE_SYSTEMCONTROLLER_H
#define FIRMWARE_SYSTEMCONTROLLER_H

#include <optional.hpp>
#include "Controller/Core0/Protocol/lcc_protocol.h"
#include "Controller/Core0/Protocol/control_board_protocol.h"
#include <SystemStatus.h>
#include "Controller/Core0/Util/TimedLatch.h"
#include "Controller/Core0/Util/HysteresisController.h"
#include "Controller/Core0/Util/HybridController.h"
#include <queue>
#include <types.h>
#include <hardware/uart.h>
#include <hardware/gpio.h>
#include <utils/PicoQueue.h>
#include <utils/MovingAverage.h>

class SsrStateQueueItem {
public:
    SsrState state = BOTH_SSRS_OFF;
};

class SystemController {
public:
    explicit SystemController(
            uart_inst_t * _uart,
            PicoQueue<SystemControllerStatusMessage> *outgoingQueue,
            PicoQueue<SystemControllerCommand> *incomingQueue
            );

    void loop();
    void sendSafePacketNoWait();
private:
    SystemControllerBailReason bail_reason = BAIL_REASON_NONE;
    SystemControllerInternalState internalState = NOT_STARTED_YET;
    SystemControllerRunState runState = RUN_STATE_UNDETEMINED;

    LccRawPacket safeLccRawPacket;

    // Feed forward PID addition
    // y = kx+m, 0 <= y <= 10
    // x is time in milliseconds since start of brew
    //
    // Start at 5 and go to zero at 20000 milliseconds
    float feedForwardK = -0.00025f;
    float feedForwardM = 5.0f;

    nonstd::optional<absolute_time_t> core1RebootTimer{};
    nonstd::optional<absolute_time_t> unbailTimer{};
    nonstd::optional<absolute_time_t> heatupStage2Timer{};
    nonstd::optional<absolute_time_t> brewStartedAt{};
    nonstd::optional<absolute_time_t> plannedAutoSleepAt{};

    uart_inst_t* uart;
    PicoQueue<SystemControllerStatusMessage> *outgoingQueue;
    PicoQueue<SystemControllerCommand> *incomingQueue;
    SystemSettings *settings;

    PidRuntimeParameters brewPidRuntimeParameters{};
    PidRuntimeParameters servicePidRuntimeParameters{};

    MovingAverage<float> brewTempAverage = MovingAverage<float>(5);
    MovingAverage<float> serviceTempAverage = MovingAverage<float>(5);

    LccParsedPacket currentLccParsedPacket;
    ControlBoardParsedPacket currentControlBoardParsedPacket;
    ControlBoardRawPacket currentControlBoardRawPacket;

    SystemControllerCoalescedState externalState();

    uint16_t bailCounter = 0;
    void softBail(SystemControllerBailReason reason);
    void hardBail(SystemControllerBailReason reason);
    inline bool isBailed() { return internalState == SOFT_BAIL || internalState == HARD_BAIL; }
    inline bool isSoftBailed() { return internalState == SOFT_BAIL; };
    void unbail();


    inline bool onlySendSafePackages() { return isBailed() || internalState == NOT_STARTED_YET; }
    [[nodiscard]] inline bool shouldForceHysteresisForBrewBoiler() const { return runState == RUN_STATE_HEATUP_STAGE_1 || runState == RUN_STATE_HEATUP_STAGE_2; };

    void setSleepMode(bool sleepMode);
    void setAutoSleepMinutes(float minutes);

    [[nodiscard]] bool areTemperaturesAtSetPoint() const;

    void initiateHeatup();
    void transitionToHeatupStage2();
    void finishHeatup();

    LccParsedPacket handleControlBoardPacket(ControlBoardParsedPacket packet);

    HybridController brewBoilerController;
    HysteresisController serviceBoilerController;

    FlowMode flowMode = PUMP_ON_SOLENOID_OPEN;

    PicoQueue<SsrState> ssrStateQueue = PicoQueue<SsrState>(25);

    TimedLatch waterTankEmptyLatch = TimedLatch(1000, false);
    TimedLatch serviceBoilerLowLatch = TimedLatch(500, false);

    void handleCommands();
    void updateControllerSettings();

    void sendLccPacket();

    void updatePlannedAutoSleep();

    void handleRunningStateAutomations();

    void onBrewStarted();
    void onBrewEnded();

    void onSleepModeEntered();
    void onSleepModeExited();

    void updateForFlowMode(LccParsedPacket* parsedPacket);
};


#endif //FIRMWARE_SYSTEMCONTROLLER_H
