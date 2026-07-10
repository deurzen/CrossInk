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
  enum class ScriptActionType : uint8_t {
    Press,
    Release,
    Render,
    Pause,
    EnableWordInboxScreenshots,
    EnableDictionaryLookup,
    EnableWordInbox
  };

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

  static bool dictionaryLookupEnabled() { return std::getenv("CROSSINK_SIMULATOR_SMOKE_DICTIONARY") != nullptr; }

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

  static ScriptAction enableDictionaryLookup() {
    return {ScriptActionType::EnableDictionaryLookup, MappedInputManager::Button::Back, nullptr, 0};
  }

  static ScriptAction enableWordInbox() {
    return {ScriptActionType::EnableWordInbox, MappedInputManager::Button::Back, nullptr, 0};
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

    if (dictionaryLookupEnabled()) {
      inputScript.push_back(enableDictionaryLookup());
      addTap(MappedInputManager::Button::Power);
      inputScript.push_back(render("Dictionary shortlist", 4));
      addTap(MappedInputManager::Button::Confirm);
      inputScript.push_back(render("Dictionary definition", 4));
      addTap(MappedInputManager::Button::Back);
      inputScript.push_back(render("Dictionary shortlist after definition", 3));
      addTap(MappedInputManager::Button::Back);
      inputScript.push_back(render("Reader after dictionary lookup", 4));
    }

    inputScript.push_back(enableWordInbox());
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

    constexpr uint32_t BULK_CONTEXT_COUNT = 500;
    WordInboxCapture bulkCapture;
    bulkCapture.bookType = WordInboxBookType::Epub;
    bulkCapture.bookPath = "/books/word-inbox-index-scale.epub";
    bulkCapture.title = "Word Inbox Index Scale";
    bulkCapture.text = "bulk context";
    for (uint32_t expectedId = 1; expectedId <= BULK_CONTEXT_COUNT; ++expectedId) {
      uint32_t captureId = 0;
      if (WordInboxStore::save(bulkCapture, captureId) != WordInboxSaveResult::Saved || captureId != expectedId) {
        fail("Word Inbox bulk index write failed at %lu", static_cast<unsigned long>(expectedId));
      }
    }

    ValidationState bulkState;
    if (!WordInboxStore::visitBooks(&bulkState, visitor) || !bulkState.found ||
        bulkState.book.contextCount != BULK_CONTEXT_COUNT || bulkState.book.latestContextId != BULK_CONTEXT_COUNT) {
      fail("Word Inbox 500-context index enumeration failed");
    }

    // Existing installations have no index. Removing both copies exercises the
    // one-time directory rebuild without changing the context format.
    char indexPath[96];
    snprintf(indexPath, sizeof(indexPath), "/.crosspoint/word_inbox/%s/index.bin", bulkState.book.key);
    Storage.remove(indexPath);
    snprintf(indexPath, sizeof(indexPath), "/.crosspoint/word_inbox/%s/index.bin.bak", bulkState.book.key);
    Storage.remove(indexPath);
    ValidationState rebuiltState;
    if (!WordInboxStore::visitBooks(&rebuiltState, visitor) || !rebuiltState.found ||
        rebuiltState.book.contextCount != BULK_CONTEXT_COUNT) {
      fail("Word Inbox index migration rebuild failed");
    }

    // Simulate power loss after a context was hidden for deletion but before
    // the index update. The next lookup must finish the pending operation.
    char pendingPath[96];
    char contextPath[96];
    char deletionPath[96];
    snprintf(pendingPath, sizeof(pendingPath), "/.crosspoint/word_inbox/%s/delete.pending", rebuiltState.book.key);
    snprintf(contextPath, sizeof(contextPath), "/.crosspoint/word_inbox/%s/00000300.ctx", rebuiltState.book.key);
    snprintf(deletionPath, sizeof(deletionPath), "/.crosspoint/word_inbox/%s/00000300.ctx.del", rebuiltState.book.key);
    HalFile pendingDelete = Storage.open(pendingPath, O_WRONLY | O_CREAT | O_TRUNC);
    const uint32_t pendingId = 300;
    if (!pendingDelete || pendingDelete.write(&pendingId, sizeof(pendingId)) != sizeof(pendingId) ||
        !pendingDelete.sync()) {
      fail("Could not create pending Word Inbox deletion fixture");
    }
    pendingDelete.close();
    if (!Storage.rename(contextPath, deletionPath)) fail("Could not hide pending Word Inbox context fixture");
    WordInboxContextInfo afterPendingDelete;
    if (!WordInboxStore::getContext(rebuiltState.book.key, 301, afterPendingDelete) ||
        afterPendingDelete.position != 300 || afterPendingDelete.previousId != 299 ||
        afterPendingDelete.contextCount != BULK_CONTEXT_COUNT - 1) {
      fail("Word Inbox pending deletion recovery failed");
    }

    const unsigned long lookupStarted = millis();
    WordInboxContextInfo middle;
    if (!WordInboxStore::getContext(rebuiltState.book.key, 250, middle) || middle.position != 250 ||
        middle.previousId != 249 || middle.nextId != 251 || middle.contextCount != BULK_CONTEXT_COUNT - 1) {
      fail("Word Inbox indexed middle lookup failed");
    }
    const unsigned long lookupMs = millis() - lookupStarted;
    if (!WordInboxStore::deleteContext(rebuiltState.book.key, 250)) {
      fail("Word Inbox indexed middle deletion failed");
    }
    WordInboxContextInfo afterMiddleDelete;
    if (!WordInboxStore::getContext(rebuiltState.book.key, 251, afterMiddleDelete) ||
        afterMiddleDelete.position != 250 || afterMiddleDelete.previousId != 249 ||
        afterMiddleDelete.contextCount != BULK_CONTEXT_COUNT - 2) {
      fail("Word Inbox indexed navigation after deletion failed");
    }

    // Corrupt the newest generation. The retained backup still references the
    // deleted neighbor, so lookup must detect that mismatch and rebuild safely.
    snprintf(indexPath, sizeof(indexPath), "/.crosspoint/word_inbox/%s/index.bin", rebuiltState.book.key);
    if (!Storage.writeFile(indexPath, "bad")) fail("Could not corrupt Word Inbox index fixture");
    WordInboxContextInfo recovered;
    if (!WordInboxStore::getContext(rebuiltState.book.key, 251, recovered) || recovered.position != 250 ||
        recovered.previousId != 249 || recovered.contextCount != BULK_CONTEXT_COUNT - 2) {
      fail("Word Inbox corrupt-index recovery failed");
    }
    if (!WordInboxStore::deleteBook(rebuiltState.book.key)) {
      fail("Word Inbox indexed test book deletion failed");
    }
    LOG_INF("SMOKE", "Validated Word Inbox index with 500 contexts (middle lookup %lums)", lookupMs);
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
      case ScriptActionType::EnableDictionaryLookup:
        SETTINGS.shortPwrBtn = CrossPointSettings::SHORT_PWRBTN::DICTIONARY_LOOKUP;
        break;
      case ScriptActionType::EnableWordInbox:
        SETTINGS.shortPwrBtn = CrossPointSettings::SHORT_PWRBTN::SAVE_WORD_INBOX;
        break;
    }
  }
};

SimulatorSmokeTest smokeTest;

}  // namespace

void runSimulatorSmokeTestTick() { smokeTest.tick(); }

#endif
