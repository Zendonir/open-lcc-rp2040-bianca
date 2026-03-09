//
// Created by Magnus Nordlander on 2023-10-29.
//

#include <cmath>
#include "Automations.h"
#include "utils/USBDebug.h"

void Automations::loop(SystemControllerStatusMessage sm) {
    if (!plannedAutoSleepAt.has_value()) {
        resetPlannedSleep();
    } else if (!settingsManager->getSleepMode() && time_reached(plannedAutoSleepAt.value())) {
        settingsManager->setSleepMode(true);
    }

    if (!plannedAutoStandbyAt.has_value()) {
        resetPlannedStandby();
    } else if (!settingsManager->getStandbyMode() && time_reached(plannedAutoStandbyAt.value())) {
        settingsManager->setSleepMode(false);
        settingsManager->setStandbyMode(true);
    }

    if (sm.currentlyBrewing && !previouslyBrewing) {
        onBrewStarted();
    } else if (previouslyBrewing && !sm.currentlyBrewing) {
        onBrewEnded();
    }

    if (!sm.sleepMode && previouslyAsleep) {
        resetPlannedSleep();
    }

    if (!sm.standbyMode && previouslyInStandby) {
        resetPlannedStandby();
    }

    if (previousAutosleepMinutes != settingsManager->getAutoSleepMin()) {
        resetPlannedSleep();
    }

    if (previousAutostandbyMinutes != settingsManager->getAutoStandbyMin()) {
        resetPlannedStandby();
    }

    previouslyBrewing = sm.currentlyBrewing;
    previouslyAsleep = sm.sleepMode;
    previouslyInStandby = sm.standbyMode;
    previousAutosleepMinutes = settingsManager->getAutoSleepMin();
    previousAutostandbyMinutes = settingsManager->getAutoStandbyMin();

    handleCurrentAutomationStep(sm);
}

void Automations::resetPlannedSleep() {
    if (settingsManager->getAutoSleepMin() > 0) {
        uint32_t ms = (uint32_t)settingsManager->getAutoSleepMin() * 60 * 1000;
        plannedAutoSleepAt = delayed_by_ms(get_absolute_time(), ms);
    } else {
        plannedAutoSleepAt.reset();
    }
}

void Automations::resetPlannedStandby() {
    if (settingsManager->getAutoStandbyMin() > 0) {
        uint32_t ms = (uint32_t)settingsManager->getAutoStandbyMin() * 60 * 1000;
        plannedAutoStandbyAt = delayed_by_ms(get_absolute_time(), ms);
    } else {
        plannedAutoStandbyAt.reset();
    }
}

void Automations::onBrewStarted() {
    brewStartedAt = get_absolute_time();

    // Starting a brew exits sleep mode
    if (settingsManager->getSleepMode()) {
        settingsManager->setSleepMode(false);
    }

    // Starting a brew also exits standby mode
    if (settingsManager->getStandbyMode()) {
        settingsManager->setStandbyMode(false);
    }

    resetPlannedSleep();
    resetPlannedStandby();
}

Automations::Automations(SettingsManager *settingsManager, PicoQueue<SystemControllerCommand> *commandQueue)
    : settingsManager(settingsManager), commandQueue(commandQueue) {
    previouslyAsleep = settingsManager->getSleepMode();
    previouslyInStandby = settingsManager->getStandbyMode();
    previousAutosleepMinutes = settingsManager->getAutoSleepMin();
    previousAutostandbyMinutes = settingsManager->getAutoStandbyMin();

    currentRoutine.emplace_back(); // Step 0

    auto fullFlow = SystemControllerCommand{
            .type = COMMAND_SET_FLOW_MODE,
            .int1 = PUMP_ON_SOLENOID_OPEN,
    };

    auto lowFlow = SystemControllerCommand{
            .type = COMMAND_SET_FLOW_MODE,
            .int1 = PUMP_OFF_SOLENOID_OPEN,
    };

    RoutineStep step1 = RoutineStep();
    step1.exitConditions.emplace_back(BREW_START, 0, 2);
    currentRoutine.push_back(step1);

    RoutineStep step2 = RoutineStep();
    step2.entryCommands.push_back(fullFlow);
    step2.exitConditions.emplace_back(BREW_TIME_ABSOLUTE, 4, 3);
    currentRoutine.push_back(step2);

    RoutineStep step3 = RoutineStep();
    step3.entryCommands.push_back(lowFlow);
    step3.exitConditions.emplace_back(STEP_TIME, 10, 4);
    currentRoutine.push_back(step3);

    RoutineStep step4 = RoutineStep();
    step4.entryCommands.push_back(fullFlow);
    currentRoutine.push_back(step4);
}

float Automations::getPlannedSleepInMinutes() {
    float sleepSeconds = plannedAutoSleepAt.has_value()
                         ? (float)(absolute_time_diff_us(get_absolute_time(), plannedAutoSleepAt.value())) / 1000.f / 1000.f
                         : INFINITY;
    if (sleepSeconds < 0) {
        sleepSeconds = 0.f;
    }

    return sleepSeconds;
}

float Automations::getPlannedStandbyInMinutes() {
    float standbySeconds = plannedAutoStandbyAt.has_value()
                           ? (float)(absolute_time_diff_us(get_absolute_time(), plannedAutoStandbyAt.value())) / 1000.f / 1000.f
                           : INFINITY;
    if (standbySeconds < 0) {
        standbySeconds = 0.f;
    }

    return standbySeconds;
}

void Automations::handleCurrentAutomationStep(SystemControllerStatusMessage sm) {
    if (currentAutomationStep >= currentRoutine.size()) {
        moveToAutomationStep(0);
        return;
    }

    auto currentStep = currentRoutine.at(currentAutomationStep);

    for (auto exitCondition : currentStep.exitConditions) {
        switch (exitCondition.type) {
            case BREW_START:
                if (sm.currentlyBrewing) {
                    moveToAutomationStep(exitCondition.exitToStep);
                    return;
                }
                break;
            case BREW_TIME_ABSOLUTE:
                if (currentBrewTime() >= exitCondition.value) {
                    moveToAutomationStep(exitCondition.exitToStep);
                    return;
                }
                break;
            case STEP_TIME:
                if (currentStepTime() >= exitCondition.value) {
                    moveToAutomationStep(exitCondition.exitToStep);
                    return;
                }
                break;
        }
    }
}

void Automations::moveToAutomationStep(uint16_t step) {
    USB_PRINTF("Moving to automation step %u\n", step);
    if (step >= currentRoutine.size()) {
        step = 0;
    }

    auto nextStep = currentRoutine.at(step);

    for (auto entryCommand : nextStep.entryCommands) {
        commandQueue->addBlocking(&entryCommand);
    }

    currentAutomationStep = step;
    currentStepStartedAt = get_absolute_time();

    if (step == 0) {
        unloadRoutine();
    }
}

void Automations::onBrewEnded() {
    brewStartedAt.reset();
    resetPlannedSleep();
    resetPlannedStandby();

    if (currentAutomationStep > 0) {
        moveToAutomationStep(0);
    }
}

void Automations::enqueueRoutine(uint32_t routineId) {
    currentlyLoadedRoutine = routineId;
    moveToAutomationStep(1);
}

void Automations::cancelRoutine() {
    moveToAutomationStep(0);
}

void Automations::exitingSleep() {
    resetPlannedSleep();
}

void Automations::unloadRoutine() {
    currentlyLoadedRoutine = 0;
    currentAutomationStep = 0;
    currentStepStartedAt.reset();
}