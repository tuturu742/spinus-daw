#include <catch2/catch_test_macros.hpp>

#include "FakeProvider.h"
#include "ui/AiSettingsDialog.h"
#include "ui/Theme.h"

#include <cstdlib>

using namespace spinus;
using namespace spinus::ui;
using spinus::test::waitFor;

namespace
{
struct TempFolder
{
    TempFolder() { root.createDirectory(); }
    ~TempFolder() { root.deleteRecursively(); }
    juce::File root = juce::File::getSpecialLocation (juce::File::tempDirectory)
                          .getChildFile (juce::String ("spinus-engine-tests") + "-" + juce::Uuid().toString());
};

/** The AI service installs with a stand-in uv and runs a stand-in PyTorch (ai/tests/fakes): nothing is
    downloaded, and the "GPU" is whatever FAKE_GPU says (none here, so the CPU build is the one used). */
struct FakePyTorch
{
    FakePyTorch()
    {
        const auto source = juce::File (SPINUS_TEST_FILES_DIR).getParentDirectory();
        const auto fakes = source.getChildFile ("ai/tests/fakes");
        oldPath = juce::SystemStats::getEnvironmentVariable ("PATH", {});
        ::setenv ("PATH", (fakes.getFullPathName() + ":" + oldPath).toRawUTF8(), 1);
        ::setenv ("FAKE_TORCH", fakes.getFullPathName().toRawUTF8(), 1);
        ::setenv ("REAL_PYTHON", source.getChildFile ("ai/.venv/bin/python").getFullPathName().toRawUTF8(), 1);
        ::setenv ("FAKE_GPU", "none", 1);
    }
    ~FakePyTorch()
    {
        ::setenv ("PATH", oldPath.toRawUTF8(), 1);
        for (auto* name : { "FAKE_TORCH", "REAL_PYTHON", "FAKE_GPU" })
            ::unsetenv (name);
    }
    juce::String oldPath;
};

int rowOf (const AnalysisEnginePage& page, const juce::String& id)
{
    const auto& builds = page.getBuilds();
    for (size_t i = 0; i < builds.size(); ++i)
        if (builds[i].id == id)
            return (int)i;
    return -1;
}
} // namespace

TEST_CASE ("Analysis engine: shows the builds, asks before installing, installs, tests and removes", "[ui][ai]")
{
    if (ai::AiService::findInstallation().python == juce::File())
        SKIP ("the AI service's Python environment is not set up (uv sync in ai/)");

    TempFolder t;
    FakePyTorch fake;
    ai::AiService service (t.root, t.root.getChildFile ("models"));
    AiSettingsDialog settings (service);
    settings.setAiOn (true);
    settings.showAnalysisEngine (true);
    auto* page = settings.getAnalysisEnginePage();
    REQUIRE (page != nullptr);
    REQUIRE (waitFor ([&] { return ! page->isBusy() && ! page->getBuilds().empty(); }));

    // The build suggested for this computer comes first; the CPU build is always offered.
    const auto cpu = rowOf (*page, "cpu");
    REQUIRE (cpu >= 0);
    CHECK (page->getBuilds()[(size_t)cpu].tested);
    CHECK (page->getButtonText (cpu).startsWith ("Install ("));
    CHECK (page->getGpuText().isNotEmpty());
    CHECK (page->getUsesText().startsWith ("Not installed yet"));

    // Nothing is downloaded until the user agrees to the size.
    juce::String question;
    std::function<void (int)> answer;
    page->setMessageBoxHandler (
        [&] (const juce::MessageBoxOptions& options, std::function<void (int)> done)
        {
            question = options.getMessage();
            answer = std::move (done);
        });
    page->askToInstall (cpu);
    CHECK (question.contains ("will download about"));
    CHECK (question.contains ("pytorch.org"));
    CHECK (! page->isInstalling());
    answer (1); // Cancel
    CHECK (! page->isInstalling());
    page->askToInstall (cpu);
    answer (0); // Install
    CHECK (page->isInstalling());
    REQUIRE (waitFor ([&] { return ! page->isBusy() && page->getBuilds()[(size_t)rowOf (*page, "cpu")].installed; }));
    CHECK (page->getStatusText() == "Installed. Spinus can analyze songs now.");
    CHECK (page->getUsesText().startsWith ("In use: CPU only, on the CPU."));
    CHECK (page->getButtonText (rowOf (*page, "cpu")) == "Installed");

    page->runTest();
    REQUIRE (waitFor ([&] { return ! page->isBusy(); }));
    CHECK (page->getStatusText().startsWith ("Test passed on the CPU (PyTorch 2.14.0+fake)."));

    page->setForceCpu (true);
    REQUIRE (waitFor ([&] { return ! page->isBusy(); }));
    page->remove (rowOf (*page, "cpu"));
    REQUIRE (waitFor ([&] { return ! page->isBusy() && ! page->getBuilds()[(size_t)rowOf (*page, "cpu")].installed; }));
    CHECK (page->getUsesText().startsWith ("Not installed yet"));
    CHECK (! t.root.getChildFile ("python/cpu").exists());

    // A window opened again follows an installation still going on, and can stop it.
    page->install (rowOf (*page, "cpu"));
    CHECK (page->isInstalling());
    settings.showAnalysisEngine (false);
    settings.showAnalysisEngine (true);
    page = settings.getAnalysisEnginePage();
    REQUIRE (waitFor ([&] { return page->isInstalling(); }));
    page->cancelInstall();
    REQUIRE (waitFor ([&] { return ! page->isBusy(); }));
    CHECK (page->getStatusText() == "Stopped.");
    CHECK (! page->getBuilds()[(size_t)rowOf (*page, "cpu")].installed);

    settings.setAiOn (false);
    CHECK (settings.getAnalysisEnginePage() == nullptr);
}

TEST_CASE ("Analysis engine screenshot for the manual", "[.screenshot][ai]")
{
    TempFolder t;
    FakePyTorch fake;
    SpinusLookAndFeel lookAndFeel { Theme::dark() };
    ai::AiService service (t.root, t.root.getChildFile ("models"));
    AiSettingsDialog settings (service);
    settings.setLookAndFeel (&lookAndFeel);
    settings.setAiOn (true);
    settings.showAnalysisEngine (true);
    auto* page = settings.getAnalysisEnginePage();
    REQUIRE (waitFor ([&] { return ! page->isBusy() && ! page->getBuilds().empty(); }));

    // The manual shows the folder a user's Spinus uses (with ~ for the home folder), not this test's
    // temporary one.
    const auto home = juce::File::getSpecialLocation (juce::File::userHomeDirectory);
    const auto python = ai::AiService::getDefaultModelsFolder().getSiblingFile ("python");
    const auto shown = python.isAChildOf (home) ? "~/" + python.getRelativePathFrom (home) : python.getFullPathName();
    bool replaced = false;
    for (auto* child : page->getChildren())
        if (auto* label = dynamic_cast<juce::Label*> (child);
            label != nullptr && label->getText().startsWith ("Kept in "))
        {
            label->setText ("Kept in " + shown + ", shared by all projects.", juce::dontSendNotification);
            replaced = true;
        }
    REQUIRE (replaced);

    const auto image = settings.createComponentSnapshot (settings.getLocalBounds(), true, 1.0f);
    const auto file =
        juce::File (SPINUS_TEST_FILES_DIR).getParentDirectory().getChildFile ("docs/user/images/analysis-engine.png");
    file.deleteFile();
    juce::FileOutputStream stream (file);
    REQUIRE (juce::PNGImageFormat().writeImageToStream (image, stream));
    settings.setLookAndFeel (nullptr);
    settings.setAiOn (false);
}
