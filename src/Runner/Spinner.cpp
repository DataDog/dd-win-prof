#include "Spinner.h"
#include "Helpers.h"

void Spinner::Run(int durationMS)
{
    Spin(durationMS);
    StaticRun(durationMS);
}

void Spinner::StaticRun(int durationMS)
{
    Spin(durationMS);
}
