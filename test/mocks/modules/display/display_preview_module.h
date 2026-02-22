#pragma once

class DisplayPreviewModule {
public:
    bool running = false;
    int updateCalls = 0;
    int cancelCalls = 0;

    bool isRunning() const { return running; }
    void setRunning(bool value) { running = value; }

    void update() { updateCalls++; }
    void cancel() {
        cancelCalls++;
        running = false;
    }
};
