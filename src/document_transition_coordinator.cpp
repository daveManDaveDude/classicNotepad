#include "document_transition_coordinator.h"

namespace classic_notepad {

bool DocumentTransitionCoordinator::TryQueue(DocumentTransitionIntent intent, std::uint64_t saveToken) noexcept
{
    if (intent == DocumentTransitionIntent::None || saveToken == 0U || pending_.IsPending()) {
        return false;
    }

    pending_ = {intent, saveToken};
    return true;
}

PendingDocumentTransition DocumentTransitionCoordinator::Pending() const noexcept
{
    return pending_;
}

PendingDocumentTransition DocumentTransitionCoordinator::TakePending() noexcept
{
    const PendingDocumentTransition pending = pending_;
    Clear();
    return pending;
}

void DocumentTransitionCoordinator::Clear() noexcept
{
    pending_ = {};
}

bool DocumentTransitionCoordinator::TryAcceptOperationNotification(
    std::uint64_t notificationToken,
    std::uint64_t currentOperationToken,
    bool operationCompleted) noexcept
{
    if (notificationToken == 0U || notificationToken != currentOperationToken ||
        notificationToken <= lastHandledOperationToken_ || !operationCompleted) {
        return false;
    }

    lastHandledOperationToken_ = notificationToken;
    return true;
}

TransitionRequestSaveDisposition ClassifyTransitionRequestSave(
    const TransitionSaveObservation& observation) noexcept
{
    if (observation.operationInProgress && observation.isSave) {
        return TransitionRequestSaveDisposition::WaitForSave;
    }
    if (!observation.operationCompleted || !observation.isSave) {
        return TransitionRequestSaveDisposition::NoSaveCompletion;
    }
    if (!observation.succeeded) {
        return TransitionRequestSaveDisposition::SaveFailed;
    }
    return observation.documentModified
        ? TransitionRequestSaveDisposition::NoSaveCompletion
        : TransitionRequestSaveDisposition::Saved;
}

PendingTransitionCompletion ClassifyPendingTransitionCompletion(
    PendingDocumentTransition pending,
    const TransitionSaveObservation& observation) noexcept
{
    if (!pending.IsPending() || observation.operationInProgress || !observation.operationCompleted ||
        !observation.isSave || observation.operationToken != pending.saveToken) {
        return PendingTransitionCompletion::NotReady;
    }
    if (!observation.succeeded) {
        return PendingTransitionCompletion::SaveFailed;
    }
    return observation.documentModified
        ? PendingTransitionCompletion::DocumentChangedDuringSave
        : PendingTransitionCompletion::PerformTransition;
}

} // namespace classic_notepad
