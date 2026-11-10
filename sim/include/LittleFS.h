#pragma once
// The UI only names LittleFS for the CYD's file-based album art path, which the simulator never takes.
struct LittleFSStub {};
inline LittleFSStub LittleFS;
