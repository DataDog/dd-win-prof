#pragma once

class Spinner
{
public:
    void Run(int durationMS);

private:
    static void StaticRun(int durationMS);
};

