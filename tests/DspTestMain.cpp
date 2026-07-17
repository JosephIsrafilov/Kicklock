#include "TestCommon.h"

#include <chrono>

int main (int argc, char** argv)
{
    // Print each test name as it starts so we can see which one crashes
    // before the runner collects results (crash = no results printed).
    class FlushingRunner : public juce::UnitTestRunner
    {
    protected:
        void logMessage (const juce::String& msg) override
        {
            if (msg.isNotEmpty())
            {
                std::cout << msg.toStdString() << std::endl;
                std::cout.flush();
            }
        }
    };

    FlushingRunner runner;
    runner.setAssertOnFailure (false);

    const auto totalStart = std::chrono::steady_clock::now();
    if (argc == 1)
    {
        runner.runAllTests();
    }
    else
    {
        std::vector<juce::String> requestedCategories;
        for (int argument = 1; argument < argc; ++argument)
        {
            const juce::String category (argv[argument]);
            if (std::find (requestedCategories.begin(), requestedCategories.end(), category)
                != requestedCategories.end())
                continue;
            requestedCategories.push_back (category);
            const auto categoryStart = std::chrono::steady_clock::now();
            runner.runTestsInCategory (category);
            const auto elapsed = std::chrono::duration<double> (
                std::chrono::steady_clock::now() - categoryStart).count();
            std::cout << "CATEGORY " << category.toStdString() << " ELAPSED: "
                      << elapsed << " s" << std::endl;
        }
    }

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

    const auto totalElapsed = std::chrono::duration<double> (
        std::chrono::steady_clock::now() - totalStart).count();
    std::cout << "TOTAL FAILURES: " << failures << std::endl;
    std::cout << "TOTAL ELAPSED: " << totalElapsed << " s" << std::endl;
    return failures > 0 ? 1 : 0;
}

