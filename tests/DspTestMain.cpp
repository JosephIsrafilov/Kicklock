#include "TestCommon.h"

int main (int argc, char** argv)
{
    juce::UnitTestRunner runner;
    runner.setAssertOnFailure (false);

    if (argc > 1)
        runner.runTestsWithName (argv[1]);
    else
        runner.runAllTests();

    int failures = 0;

    for (int i = 0; i < runner.getNumResults(); ++i)
    {
        const auto* result = runner.getResult (i);
        if (result == nullptr)
            continue;

        std::cout << "[" << (result->failures > 0 ? "FAIL" : "ok  ") << "] "
                  << result->unitTestName.toStdString() << " / "
                  << result->subcategoryName.toStdString()
                  << "  (" << result->passes << " passed, "
                  << result->failures << " failed)" << std::endl;

        for (const auto& msg : result->messages)
            if (msg.isNotEmpty())
                std::cout << "        " << msg.toStdString() << std::endl;

        failures += result->failures;
    }

    std::cout << "TOTAL FAILURES: " << failures << std::endl;
    return failures > 0 ? 1 : 0;
}

