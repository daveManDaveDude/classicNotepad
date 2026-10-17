#pragma once

#include <cstdint>

namespace classic_notepad {

enum class DocumentTransitionIntent : std::uint8_t {
    None,
    NewDocument,
    OpenDialog,
    CloseWindow,
};

struct PendingDocumentTransition {
    DocumentTransitionIntent intent = DocumentTransitionIntent::None;
    std::uint64_t saveToken = 0;

    bool IsPending() const noexcept
    {
        return intent != DocumentTransitionIntent::None;
    }
};

struct TransitionSaveObservation {
    bool operationInProgress = false;
    bool operationCompleted = false;
    bool isSave = false;
    std::uint64_t operationToken = 0;
    bool succeeded = false;
    bool documentModified = false;
};

enum class TransitionRequestSaveDisposition : std::uint8_t {
    NoSaveCompletion,
    WaitForSave,
    Saved,
    SaveFailed,
};

enum class PendingTransitionCompletion : std::uint8_t {
    NotReady,
    PerformTransition,
    SaveFailed,
    DocumentChangedDuringSave,
};

class DocumentTransitionCoordinator {
public:
    bool TryQueue(DocumentTransitionIntent intent, std::uint64_t saveToken) noexcept;
    PendingDocumentTransition Pending() const noexcept;
    PendingDocumentTransition TakePending() noexcept;
    void Clear() noexcept;
    bool TryAcceptOperationNotification(
        std::uint64_t notificationToken,
        std::uint64_t currentOperationToken,
        bool operationCompleted) noexcept;

private:
    PendingDocumentTransition pending_;
    std::uint64_t lastHandledOperationToken_ = 0;
};

TransitionRequestSaveDisposition ClassifyTransitionRequestSave(
    const TransitionSaveObservation& observation) noexcept;

PendingTransitionCompletion ClassifyPendingTransitionCompletion(
    PendingDocumentTransition pending,
    const TransitionSaveObservation& observation) noexcept;

} // namespace classic_notepad
