#include "SystemCommandsModule.h"
#include "input/InputBroker.h"
#include "meshUtils.h"

#if HAS_SCREEN
#include "MessageStore.h"
#include "graphics/Screen.h"
#include "graphics/SharedUIDisplay.h"
#endif

#include "GPS.h"
#include "MeshService.h"
#include "Module.h"
#include "NodeDB.h"
#include "main.h"
#include "modules/AdminModule.h"
#include "modules/ExternalNotificationModule.h"

SystemCommandsModule *systemCommandsModule;

SystemCommandsModule::SystemCommandsModule()
{
    if (inputBroker)
        inputObserver.observe(inputBroker);
}

int SystemCommandsModule::handleInputEvent(const InputEvent *event)
{
    LOG_INPUT("SystemCommands Input event %u! kb %u", event->inputEvent, event->kbchar);
    // System commands (all others fall through)
    switch (event->kbchar) {
    // Fn key symbols
    case INPUT_BROKER_MSG_FN_SYMBOL_ON:
    case INPUT_BROKER_MSG_FN_SYMBOL_OFF:
        return 0;
    // Brightness
    case INPUT_BROKER_MSG_BRIGHTNESS_UP:
        IF_SCREEN(screen->increaseBrightness());
        LOG_DEBUG("Increase Screen Brightness");
        return 0;
    case INPUT_BROKER_MSG_BRIGHTNESS_DOWN:
        IF_SCREEN(screen->decreaseBrightness());
        LOG_DEBUG("Decrease Screen Brightness");
        return 0;
    // Mute
    case INPUT_BROKER_MSG_MUTE_TOGGLE:
        if (moduleConfig.external_notification.enabled && externalNotificationModule) {
            externalNotificationModule->setMute(!externalNotificationModule->getMute());
            IF_SCREEN(if (!externalNotificationModule->getMute()) externalNotificationModule->stopNow(); screen->showSimpleBanner(
                externalNotificationModule->getMute() ? "Notifications\nDisabled" : "Notifications\nEnabled", 3000);)
        }
        return 0;
    // Bluetooth
    case INPUT_BROKER_MSG_BLUETOOTH_TOGGLE:
        config.bluetooth.enabled = !config.bluetooth.enabled;
        LOG_INFO("User toggled Bluetooth");
        nodeDB->saveToDisk();
#if defined(ARDUINO_ARCH_NRF52)
        if (!config.bluetooth.enabled) {
            disableBluetooth();
            IF_SCREEN(screen->showSimpleBanner("Bluetooth OFF\nRebooting", 3000));
            rebootAtMsec = millis() + DEFAULT_REBOOT_SECONDS * 2000;
        } else {
            IF_SCREEN(screen->showSimpleBanner("Bluetooth ON\nRebooting", 3000));
            rebootAtMsec = millis() + DEFAULT_REBOOT_SECONDS * 1000;
        }
#else
        if (!config.bluetooth.enabled) {
            disableBluetooth();
            IF_SCREEN(screen->showSimpleBanner("Bluetooth OFF", 3000));
        } else {
            IF_SCREEN(screen->showSimpleBanner("Bluetooth ON\nRebooting", 3000));
            rebootAtMsec = millis() + DEFAULT_REBOOT_SECONDS * 1000;
        }
#endif
        return 0;
    case INPUT_BROKER_MSG_REBOOT:
        IF_SCREEN(screen->showSimpleBanner("Rebooting...", 0));
        nodeDB->saveToDisk();
#if HAS_SCREEN
        messageStore.saveToFlash();
#endif
        rebootAtMsec = millis() + DEFAULT_REBOOT_SECONDS * 1000;
        // runState = CANNED_MESSAGE_RUN_STATE_INACTIVE;
        return true;
    }

    switch (event->inputEvent) {
    // TrailMesh7: single press asks the paired phone to start a hands-free
    // message. Local only — `sendToPhone` enqueues straight to the BLE client
    // and never reaches the radio, so this costs no airtime, needs no GPS fix,
    // and is unaffected by the device role.
    //
    // Single press is used because it is genuinely free on a screenless tag:
    // `CannedMessageModule` is the only consumer of USER_PRESS and it disables
    // itself, without registering an input observer, when no canned messages
    // are configured and no keyboard is attached.
    //
    // Deliberately not the double press. That is the ad-hoc position ping, and
    // inferring intent from a position packet — which is what stock firmware
    // would force the app to do — cannot be told apart from a scheduled
    // broadcast without also depending on a GPS fix and a non-TRACKER role.
    case INPUT_BROKER_USER_PRESS: {
        meshtastic_MeshPacket *p = router->allocForSending();
        p->to = nodeDB->getNodeNum(); // addressed at ourselves; never transmitted
        p->decoded.portnum = meshtastic_PortNum_PRIVATE_APP;
        p->decoded.payload.size = 3;
        p->decoded.payload.bytes[0] = 0x00; // app extended-frame prefix
        p->decoded.payload.bytes[1] = 0x02; // extended type: tag trigger
        p->decoded.payload.bytes[2] = 0x01; // gesture: single press
        service->sendToPhone(p);
        LOG_INFO("TrailMesh7: sent tag-trigger to phone");
        return true;
    }
        // GPS
    case INPUT_BROKER_GPS_TOGGLE:
#if !MESHTASTIC_EXCLUDE_GPS
        if (gps) {
            if (config.position.gps_mode == meshtastic_Config_PositionConfig_GpsMode_ENABLED &&
                config.position.fixed_position == false) {
                nodeDB->clearLocalPosition();
                nodeDB->saveToDisk();
            }
            gps->toggleGpsMode();
            const char *msg =
                (config.position.gps_mode == meshtastic_Config_PositionConfig_GpsMode_ENABLED) ? "GPS Enabled" : "GPS Disabled";
            IF_SCREEN(screen->forceDisplay(); screen->showSimpleBanner(msg, 3000);)
        }
#endif
        return true;
    // Mesh ping
    case INPUT_BROKER_SEND_PING:
        service->refreshLocalMeshNode();
        if (service->trySendPosition(NODENUM_BROADCAST, true)) {
            IF_SCREEN(screen->showSimpleBanner("Position\nSent", 3000));
        } else {
            IF_SCREEN(screen->showSimpleBanner("Node Info\nSent", 3000));
        }
        return true;
    // Power control
    case INPUT_BROKER_SHUTDOWN:
        shutdownAtMsec = millis();
        return true;
    // factory reset
    case INPUT_BROKER_FACTORY_RST:
        LOG_INFO("Initiate full factory reset");
        nodeDB->factoryReset(true);
        disableBluetooth();
        // reboot(DEFAULT_REBOOT_SECONDS);
        LOG_INFO("Reboot in %d seconds", DEFAULT_REBOOT_SECONDS);
        if (screen)
            screen->showSimpleBanner("Rebooting...", 0); // stays on screen
        rebootAtMsec = (DEFAULT_REBOOT_SECONDS < 0) ? 0 : (millis() + DEFAULT_REBOOT_SECONDS * 1000);
        return true;

    default:
        // No other input events handled here
        break;
    }
    return false;
}
