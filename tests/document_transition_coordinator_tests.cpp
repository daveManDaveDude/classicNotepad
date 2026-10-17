#include "document_transition_coordinator.h"

#include <iostream>

namespace {

int g_failures = 0;

void Expect(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++g_failures;
    }
}

classic_notepad::TransitionSaveObservation SaveObservation(
    bool inProgress,
    bool completed,
    std::uint64_t token,
    bool succeeded,
    bool modified)
{
    return {inProgress, completed, true, token, succeeded, modified};
}

void TestPendingIntentAndToken()
{
    using classic_notepad::DocumentTransitionCoordinator;
    using classic_notepad::DocumentTransitionIntent;

    DocumentTransitionCoordinator coordinator;
    Expect(!coordinator.Pending().IsPending(), "coordinator starts without a pending transition");
    Expect(!coordinator.TryQueue(DocumentTransitionIntent::None, 7U), "None is not queueable");
    Expect(!coordinator.TryQueue(DocumentTransitionIntent::NewDocument, 0U), "a transition requires a nonzero save token");
    Expect(coordinator.TryQueue(DocumentTransitionIntent::NewDocument, 41U), "New can be associated with a save token");
    Expect(!coordinator.TryQueue(DocumentTransitionIntent::OpenDialog, 42U), "a second transition cannot replace a pending intent");

    const auto pending = coordinator.Pending();
    Expect(pending.intent == DocumentTransitionIntent::NewDocument, "the first queued intent is retained");
    Expect(pending.saveToken == 41U, "the associated save token is retained");

    const auto taken = coordinator.TakePending();
    Expect(taken.intent == DocumentTransitionIntent::NewDocument && taken.saveToken == 41U,
        "taking pending state returns the intent and token");
    Expect(!coordinator.Pending().IsPending(), "taking pending state clears it");

    Expect(coordinator.TryQueue(DocumentTransitionIntent::OpenDialog, 43U), "Open can be queued after prior completion");
    coordinator.Clear();
    Expect(!coordinator.Pending().IsPending(), "explicit clear removes a queued transition");
    Expect(coordinator.TryQueue(DocumentTransitionIntent::CloseWindow, 44U), "Close can be queued");
    Expect(coordinator.Pending().intent == DocumentTransitionIntent::CloseWindow, "Close intent is preserved");
}

void TestRequestSaveClassification()
{
    using classic_notepad::ClassifyTransitionRequestSave;
    using classic_notepad::TransitionRequestSaveDisposition;

    Expect(ClassifyTransitionRequestSave(SaveObservation(true, false, 5U, false, true)) ==
        TransitionRequestSaveDisposition::WaitForSave, "an active Save queues transition resumption");
    Expect(ClassifyTransitionRequestSave(SaveObservation(false, true, 5U, true, false)) ==
        TransitionRequestSaveDisposition::Saved, "a completed clean Save allows immediate transition");
    Expect(ClassifyTransitionRequestSave(SaveObservation(false, true, 5U, true, true)) ==
        TransitionRequestSaveDisposition::NoSaveCompletion, "a Save with newer edits does not transition");
    Expect(ClassifyTransitionRequestSave(SaveObservation(false, true, 5U, false, true)) ==
        TransitionRequestSaveDisposition::SaveFailed, "a failed completed Save is reported");

    auto otherOperation = SaveObservation(true, false, 5U, false, true);
    otherOperation.isSave = false;
    Expect(ClassifyTransitionRequestSave(otherOperation) == TransitionRequestSaveDisposition::NoSaveCompletion,
        "an unrelated operation is not classified as a Save");
    otherOperation = SaveObservation(false, true, 5U, true, false);
    otherOperation.isSave = false;
    Expect(ClassifyTransitionRequestSave(otherOperation) == TransitionRequestSaveDisposition::NoSaveCompletion,
        "a completed unrelated operation cannot authorize a transition");
}

void TestPendingCompletionClassification()
{
    using classic_notepad::ClassifyPendingTransitionCompletion;
    using classic_notepad::DocumentTransitionIntent;
    using classic_notepad::PendingDocumentTransition;
    using classic_notepad::PendingTransitionCompletion;

    const PendingDocumentTransition pending{DocumentTransitionIntent::NewDocument, 73U};
    Expect(ClassifyPendingTransitionCompletion({}, SaveObservation(false, true, 73U, true, false)) ==
        PendingTransitionCompletion::NotReady, "completion without pending intent is ignored");
    Expect(ClassifyPendingTransitionCompletion(pending, SaveObservation(true, true, 73U, true, false)) ==
        PendingTransitionCompletion::NotReady, "a still-running Save cannot resume the transition");
    Expect(ClassifyPendingTransitionCompletion(pending, SaveObservation(false, false, 73U, true, false)) ==
        PendingTransitionCompletion::NotReady, "an incomplete operation cannot resume the transition");
    Expect(ClassifyPendingTransitionCompletion(pending, SaveObservation(false, true, 72U, true, false)) ==
        PendingTransitionCompletion::NotReady, "a stale token cannot resume the transition");

    auto unrelated = SaveObservation(false, true, 73U, true, false);
    unrelated.isSave = false;
    Expect(ClassifyPendingTransitionCompletion(pending, unrelated) == PendingTransitionCompletion::NotReady,
        "a non-Save completion cannot resume the transition");
    Expect(ClassifyPendingTransitionCompletion(pending, SaveObservation(false, true, 73U, true, false)) ==
        PendingTransitionCompletion::PerformTransition, "a matching successful clean Save resumes the transition");
    Expect(ClassifyPendingTransitionCompletion(pending, SaveObservation(false, true, 73U, true, true)) ==
        PendingTransitionCompletion::DocumentChangedDuringSave, "newer edits keep the source document open");
    Expect(ClassifyPendingTransitionCompletion(pending, SaveObservation(false, true, 73U, false, true)) ==
        PendingTransitionCompletion::SaveFailed, "a matching failed Save rejects the transition");
}

void TestOperationNotificationAcceptance()
{
    using classic_notepad::DocumentTransitionCoordinator;

    DocumentTransitionCoordinator coordinator;
    Expect(!coordinator.TryAcceptOperationNotification(0U, 0U, true),
        "zero notification tokens are rejected");
    Expect(!coordinator.TryAcceptOperationNotification(11U, 12U, true),
        "a token that differs from the current operation is rejected");
    Expect(coordinator.TryAcceptOperationNotification(12U, 12U, true),
        "the current token can be retried after a mismatched notification");
    Expect(!coordinator.TryAcceptOperationNotification(13U, 13U, false),
        "an incomplete operation notification is rejected");
    Expect(coordinator.TryAcceptOperationNotification(13U, 13U, true),
        "a rejected incomplete notification can be retried when complete");
    Expect(!coordinator.TryAcceptOperationNotification(13U, 13U, true),
        "a duplicate notification is rejected after acceptance");
    Expect(!coordinator.TryAcceptOperationNotification(12U, 12U, true),
        "an older matching notification is rejected after a newer token was handled");

    DocumentTransitionCoordinator genericOperations;
    // The gate has no operation-name filter: Find and Replace use the same
    // completion-token acceptance contract.
    const bool findAccepted = genericOperations.TryAcceptOperationNotification(21U, 21U, true);
    const bool replaceAccepted = genericOperations.TryAcceptOperationNotification(22U, 22U, true);
    Expect(findAccepted, "a completed Find notification is accepted generically");
    Expect(replaceAccepted, "a later completed Replace notification is accepted generically");
}

} // namespace

int main()
{
    TestPendingIntentAndToken();
    TestRequestSaveClassification();
    TestPendingCompletionClassification();
    TestOperationNotificationAcceptance();
    if (g_failures != 0) {
        std::cerr << g_failures << " document transition coordinator test(s) failed.\n";
        return 1;
    }
    std::cout << "DocumentTransitionCoordinatorTests passed.\n";
    return 0;
}
