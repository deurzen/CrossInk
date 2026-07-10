#ifdef SIMULATOR

#include "SimulatorSmokeTest.h"

#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>
#include <cstdlib>
#include <exception>
#include <memory>
#include <vector>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "activities/ActivityManager.h"
#include "activities/reader/EpubReaderMenuActivity.h"
#include "activities/reader/ReaderOptionsActivity.h"
#include "components/UITheme.h"
#include "word_inbox/WordInboxStore.h"

extern ActivityManager activityManager;
extern GfxRenderer renderer;
extern MappedInputManager mappedInputManager;

namespace {

enum class SmokeStep : uint8_t {
  Start,
  Home,
  FileBrowser,
  RecentBooks,
  Settings,
  ReaderOptions,
  ReaderMenu,
  Sleep,
  Reader,
  ReaderInput,
  Done,
};

class SimulatorSmokeTest {
 public:
  void tick() {
    if (!enabled()) return;

    try {
      tickImpl();
    } catch (const std::exception& e) {
      fail("Unhandled exception: %s", e.what());
    } catch (...) {
      fail("Unhandled non-standard exception");
    }
  }

 private:
  enum class ScriptActionType : uint8_t { Press, Release, Render, Pause, EnableWordInboxScreenshots };

  struct ScriptAction {
    ScriptActionType type;
    MappedInputManager::Button button;
    const char* label;
    int settleFrames;
  };

  SmokeStep step = SmokeStep::Start;
  int settleFrames = 0;
  const char* activeStepName = nullptr;
  std::vector<ScriptAction> inputScript;
  size_t scriptIndex = 0;

  static bool enabled() { return std::getenv("CROSSINK_SIMULATOR_SMOKE_TEST") != nullptr; }

  static int pageTurnCount() {
    const char* raw = std::getenv("CROSSINK_SIMULATOR_SMOKE_PAGE_TURNS");
    if (raw == nullptr || raw[0] == '\0') {
      return 2;
    }
    return std::max(0, std::atoi(raw));
  }

  static void applyRequestedTheme() {
    const char* raw = std::getenv("CROSSINK_SIMULATOR_SMOKE_THEME");
    if (raw == nullptr || raw[0] == '\0') {
      return;
    }

    const int theme = std::atoi(raw);
    if (theme < 0 || theme >= CrossPointSettings::UI_THEME_COUNT) {
      fail("Invalid smoke test theme index: %d", theme);
    }

    SETTINGS.uiTheme = static_cast<uint8_t>(theme);
    UITheme::getInstance().reload();
    LOG_INF("SMOKE", "Using theme index %d", theme);
  }

  [[noreturn]] static void fail(const char* message) {
    LOG_ERR("SMOKE", "%s", message);
    std::_Exit(2);
  }

  template <typename... Args>
  [[noreturn]] static void fail(const char* format, Args... args) {
    logPrintf("ERR", "SMOKE", format, args...);
    logPrintf("ERR", "SMOKE", "\n");
    std::_Exit(2);
  }

  static void renderCurrentStep(const char* name) {
    LOG_INF("SMOKE", "Rendering %s", name);
    if (activityManager.requestUpdateAndWait() != RequestUpdateResult::Rendered) {
      fail("Render was rejected for %s", name);
    }
  }

  void queueStep(const char* name, SmokeStep nextStep, int framesToSettle = 3) {
    activeStepName = name;
    settleFrames = framesToSettle;
    step = nextStep;
  }

  void tickImpl() {
    mappedInputManager.simulatorClearInputFrame();

    if (settleFrames > 0) {
      --settleFrames;
      if (settleFrames == 0 && activeStepName != nullptr) {
        renderCurrentStep(activeStepName);
        activeStepName = nullptr;
      }
      return;
    }

    switch (step) {
      case SmokeStep::Start:
        LOG_INF("SMOKE", "Starting simulator smoke test");
        if (!CrossPointSettings::verifySleepTimeoutMigrationContract()) {
          fail("Sleep timeout migration contract failed");
        }
        if (!CrossPointSettings::verifySleepScreenMigrationContract()) {
          fail("Sleep screen migration contract failed");
        }
        applyRequestedTheme();
        activityManager.goHome();
        queueStep("Home", SmokeStep::Home);
        break;

      case SmokeStep::Home:
        activityManager.goToFileBrowser("/books");
        queueStep("File Browser", SmokeStep::FileBrowser);
        break;

      case SmokeStep::FileBrowser:
        activityManager.goToRecentBooks();
        queueStep("Recent Books", SmokeStep::RecentBooks);
        break;

      case SmokeStep::RecentBooks:
        activityManager.goToSettings();
        queueStep("Settings", SmokeStep::Settings);
        break;

      case SmokeStep::Settings:
        activityManager.replaceActivity(std::make_unique<ReaderOptionsActivity>(renderer, mappedInputManager));
        queueStep("Reader Options", SmokeStep::ReaderOptions);
        break;

      case SmokeStep::ReaderOptions:
        activityManager.replaceActivity(
            std::make_unique<EpubReaderMenuActivity>(renderer, mappedInputManager, "Smoke Test", 1, 1, 0,
                                                     SETTINGS.orientation, false, false, false, false, false));
        queueStep("Reader Menu", SmokeStep::ReaderMenu);
        break;

      case SmokeStep::ReaderMenu:
        activityManager.goToSleep();
        queueStep("Sleep", SmokeStep::Sleep);
        break;

      case SmokeStep::Sleep: {
        const char* bookPath = std::getenv("CROSSINK_SIMULATOR_SMOKE_BOOK");
        if (bookPath == nullptr || bookPath[0] == '\0') {
          LOG_INF("SMOKE", "Skipping Reader step; CROSSINK_SIMULATOR_SMOKE_BOOK is not set");
          step = SmokeStep::Reader;
          break;
        }
        if (!Storage.exists(bookPath)) {
          fail("Smoke test book is missing: %s", bookPath);
        }
        activityManager.goToReader(bookPath, true);
        queueStep("Reader", SmokeStep::Reader, 8);
        break;
      }

      case SmokeStep::Reader:
        buildReaderInputScript();
        step = SmokeStep::ReaderInput;
        break;

      case SmokeStep::ReaderInput:
        runReaderInputScript();
        break;

      case SmokeStep::Done:
        LOG_INF("SMOKE", "Simulator smoke test passed");
        std::_Exit(0);
    }
  }

  static ScriptAction press(MappedInputManager::Button button) { return {ScriptActionType::Press, button, nullptr, 0}; }

  static ScriptAction release(MappedInputManager::Button button) {
    return {ScriptActionType::Release, button, nullptr, 0};
  }

  static ScriptAction render(const char* label, int framesToSettle = 3) {
    return {ScriptActionType::Render, MappedInputManager::Button::Back, label, framesToSettle};
  }

  static ScriptAction pause(int framesToSettle) {
    return {ScriptActionType::Pause, MappedInputManager::Button::Back, nullptr, framesToSettle};
  }

  static ScriptAction enableWordInboxScreenshots() {
    return {ScriptActionType::EnableWordInboxScreenshots, MappedInputManager::Button::Back, nullptr, 0};
  }

  void addTap(MappedInputManager::Button button) {
    inputScript.push_back(press(button));
    inputScript.push_back(release(button));
  }

  void buildReaderInputScript() {
    inputScript.clear();
    scriptIndex = 0;

    const int turns = pageTurnCount();
    for (int i = 0; i < turns; i++) {
      addTap(MappedInputManager::Button::PageForward);
      inputScript.push_back(render("Reader after page forward", 4));
    }

    SETTINGS.shortPwrBtn = CrossPointSettings::SHORT_PWRBTN::SAVE_WORD_INBOX;
    SETTINGS.wordInboxScreenshots = 0;
    addTap(MappedInputManager::Button::Power);
    // Let the first, text-only capture toast expire without input or a page render.
    inputScript.push_back(pause(140));
    inputScript.push_back(enableWordInboxScreenshots());
    // Capture twice more with screenshots, then turn immediately while the replacement toast is visible.
    addTap(MappedInputManager::Button::Power);
    addTap(MappedInputManager::Button::Power);
    addTap(MappedInputManager::Button::PageForward);
    inputScript.push_back(render("Reader after repeated Word Inbox capture and page turn", 5));

    addTap(MappedInputManager::Button::Confirm);
    inputScript.push_back(render("Reader Menu opened from EPUB", 4));

    addTap(MappedInputManager::Button::Down);
    inputScript.push_back(render("Reader Menu Reader Options selection", 3));

    addTap(MappedInputManager::Button::Confirm);
    inputScript.push_back(render("Reader Options opened from Reader Menu", 4));

    addTap(MappedInputManager::Button::Down);
    inputScript.push_back(render("Reader Options after navigation", 3));

    addTap(MappedInputManager::Button::Confirm);
    inputScript.push_back(render("Reader Options after toggle", 3));

    addTap(MappedInputManager::Button::Back);
    inputScript.push_back(render("Reader Menu after closing Reader Options", 4));

    addTap(MappedInputManager::Button::Back);
    inputScript.push_back(render("Reader after closing Reader Menu", 4));

    LOG_INF("SMOKE", "Running reader input script with %d page turn(s)", turns);
  }

  static void validateWordInboxCapture() {
    struct ValidationState {
      bool found = false;
      WordInboxBookInfo book;
    } state;
    const auto visitor = [](void* context, const WordInboxBookInfo& book) {
      auto& validation = *static_cast<ValidationState*>(context);
      validation.found = true;
      validation.book = book;
      return false;
    };

    if (!WordInboxStore::visitBooks(&state, visitor) || !state.found || state.book.contextCount != 3 ||
        state.book.latestContextId != 3) {
      fail("Word Inbox book enumeration failed");
    }

    WordInboxContextInfo textOnlyCapture;
    char screenshotPath[96];
    if (!WordInboxStore::getContext(state.book.key, state.book.earliestContextId, textOnlyCapture) ||
        !textOnlyCapture.hasText || textOnlyCapture.hasScreenshot ||
        WordInboxStore::getScreenshotPath(state.book.key, textOnlyCapture.id, screenshotPath, sizeof(screenshotPath))) {
      fail("Word Inbox text-only context validation failed");
    }

    WordInboxContextInfo capture;
    if (!WordInboxStore::getContext(state.book.key, state.book.latestContextId, capture) || capture.position != 3 ||
        capture.contextCount != 3 || !capture.hasScreenshot || !capture.hasText || capture.textLength == 0) {
      fail("Word Inbox context metadata validation failed");
    }

    HalFile textFile;
    char firstByte = 0;
    if (!WordInboxStore::openContextText(state.book.key, capture, textFile) || textFile.read(&firstByte, 1) != 1 ||
        firstByte == 0) {
      fail("Word Inbox text streaming validation failed");
    }
    textFile.close();

    if (!WordInboxStore::getScreenshotPath(state.book.key, capture.id, screenshotPath, sizeof(screenshotPath))) {
      fail("Word Inbox screenshot validation failed");
    }
    if (!WordInboxStore::deleteContext(state.book.key, capture.id)) {
      fail("Word Inbox context deletion failed");
    }
    ValidationState afterContextDelete;
    if (!WordInboxStore::visitBooks(&afterContextDelete, visitor) || !afterContextDelete.found ||
        afterContextDelete.book.contextCount != 2) {
      fail("Word Inbox context deletion count was incorrect");
    }
    if (!WordInboxStore::deleteBook(state.book.key)) {
      fail("Word Inbox book deletion failed");
    }
    ValidationState afterBookDelete;
    if (!WordInboxStore::visitBooks(&afterBookDelete, visitor) || afterBookDelete.found) {
      fail("Deleted Word Inbox book remained visible");
    }
    LOG_INF("SMOKE", "Validated Word Inbox feedback races and context CRUD API");
  }

  void runReaderInputScript() {
    if (scriptIndex >= inputScript.size()) {
      validateWordInboxCapture();
      step = SmokeStep::Done;
      return;
    }

    const auto& action = inputScript[scriptIndex++];
    switch (action.type) {
      case ScriptActionType::Press:
        mappedInputManager.simulatorInjectPress(action.button);
        break;
      case ScriptActionType::Release:
        mappedInputManager.simulatorInjectRelease(action.button);
        break;
      case ScriptActionType::Render:
        queueStep(action.label, SmokeStep::ReaderInput, action.settleFrames);
        break;
      case ScriptActionType::Pause:
        settleFrames = action.settleFrames;
        break;
      case ScriptActionType::EnableWordInboxScreenshots:
        SETTINGS.wordInboxScreenshots = 1;
        break;
    }
  }
};

SimulatorSmokeTest smokeTest;

}  // namespace

void runSimulatorSmokeTestTick() { smokeTest.tick(); }

#endif
