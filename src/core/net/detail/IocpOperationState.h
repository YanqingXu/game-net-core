#pragma once

#ifdef _WIN32

#include "gamenet/core/net/platform/IocpOperation.h"

#include <limits>

namespace gamenet::net::detail {

inline bool prepareIocpOperationSubmission(
    IocpOperation& operation) noexcept {
    if (operation.submissionPrepared || operation.submissionAccepted ||
        operation.generation ==
            (std::numeric_limits<std::uint64_t>::max)()) {
        return false;
    }
    ++operation.generation;
    operation.submissionPrepared = true;
    operation.completionObserved = false;
    operation.bytesTransferred = 0;
    operation.error = 0;
    operation.nextPublishedCompletion = nullptr;
    return true;
}

inline bool commitIocpOperationSubmission(
    IocpOperation& operation) noexcept {
    if (operation.submissionAccepted) {
        return false;
    }
    if (!operation.submissionPrepared &&
        !prepareIocpOperationSubmission(operation)) {
        return false;
    }
    operation.submissionPrepared = false;
    operation.submissionAccepted = true;
    return true;
}

inline bool rejectIocpOperationSubmission(
    IocpOperation& operation) noexcept {
    if (!operation.submissionPrepared || operation.submissionAccepted) {
        return false;
    }
    operation.submissionPrepared = false;
    operation.completionObserved = false;
    return true;
}

inline bool retireIocpOperationSubmission(
    IocpOperation& operation) noexcept {
    if (!operation.submissionAccepted || operation.generation == 0 ||
        operation.terminalGeneration == operation.generation) {
        return false;
    }
    operation.submissionAccepted = false;
    operation.submissionPrepared = false;
    operation.terminalGeneration = operation.generation;
    operation.completionObserved = true;
    return true;
}

}  // namespace gamenet::net::detail

#endif
