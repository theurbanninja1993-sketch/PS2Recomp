#ifndef PS2_DEBUG_PANEL_H
#define PS2_DEBUG_PANEL_H

class PS2Runtime;

class PS2DebugPanel
{
public:
    void initialize();
    void shutdown();
    void draw(PS2Runtime &runtime);

    bool isVisible() const { return m_visible; }
    void setVisible(bool visible) { m_visible = visible; }
    void toggleVisible() { m_visible = !m_visible; }

private:
    void drawDebugger(PS2Runtime &runtime);
    void drawSettingsMenu(PS2Runtime &runtime);
    void drawSettingsHint();

    // Key rebinding in progress (Controls tab): action and slot, -1 = none.
    int m_captureAction = -1;
    int m_captureSlot = 0;
    int m_settingsTab = 0;        // selected settings tab (L1/R1 switch tabs)
    int m_settingsTabRequest = -1; // tab to select on the next frame
    bool m_padComboLatched = false; // Select+Start toggled the menu; wait for release
    bool m_initialized = false;
    bool m_visible = false; // F1
    bool m_showRegisters = true;
    unsigned int m_memoryAddress = 0x00100000u;
    unsigned int m_memoryBytes = 0x100u;
};

#endif // PS2_DEBUG_PANEL_H
