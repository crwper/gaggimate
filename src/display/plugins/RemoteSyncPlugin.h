#ifndef REMOTESYNCPLUGIN_H
#define REMOTESYNCPLUGIN_H

#include <FS.h>
#include <SPIFFS.h>
#include <display/core/Plugin.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

class RemoteSyncPlugin : public Plugin {
  public:
    RemoteSyncPlugin() = default;

    void setup(Controller *controller, PluginManager *pluginManager) override;
    void loop() override {} // Driven by the FreeRTOS task and event notifications.

  private:
    // Wakes the task; safe to call from event handler context.
    void requestSync();

    // The FreeRTOS task body. Sleeps for the periodic interval, can be woken
    // early by `requestSync()`. Each wake runs one full sync round.
    void taskLoop();
    static void taskTrampoline(void *arg);

    // One reconciliation pass: fetch server manifest, diff, POST any shots
    // the server doesn't already have. Upload-only — server-side retention
    // is the server's concern, so we never delete from absence.
    // Returns false if anything fails — caller logs and we'll try again next tick.
    bool runSync();

    Controller *controller = nullptr;
    PluginManager *pluginManager = nullptr;
    FS *fs = &SPIFFS;
    TaskHandle_t taskHandle = nullptr;

    const char *LOG_TAG = "RemoteSyncPlugin";
};

#endif // REMOTESYNCPLUGIN_H
