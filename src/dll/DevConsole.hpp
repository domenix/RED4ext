#pragma once

#include "Config.hpp"

class DevConsole
{
public:
    DevConsole(const Config::DevConfig& aConfig);
    ~DevConsole();

    bool IsOutputRedirected() const;

private:
    bool m_isCreated;

    FILE* m_stdoutStream;
    FILE* m_stderrStream;

#ifndef _WIN32
    // Nothing is allocated on macOS; the process already has usable standard streams.
    bool m_hasInheritedConsole = false;
#endif
};
