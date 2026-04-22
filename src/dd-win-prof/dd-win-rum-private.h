// Unless explicitly stated otherwise all files in this repository are licensed under the Apache 2 License.
// This product includes software developed at Datadog (https://www.datadoghq.com/). Copyright 2025 Datadog, Inc.

#pragma once

#include "dd-win-prof.h"

typedef struct _RumSessionContext
{
    const char* application_id;  // UUID string, set once per process lifetime
    const char* session_id;      // UUID string, changes on session rotation
} RumSessionContext;

typedef struct _RumViewValues
{
    const char* view_id;   // UUID string; nullptr or "" to clear current view
    const char* view_name; // human-readable name, e.g. "HomePage"
} RumViewValues;

extern "C" {
    // Set stable RUM session context. Safe to call from any thread.
    // application_id: write-once per process. First non-empty value is stored
    // as a profile-level tag; subsequent calls with a different value return false.
    // session_id: updated on every call. Session transitions are tracked with
    // timestamps.
    // Returns false if pContext is null, application_id or session_id is missing,
    // or a different application_id is rejected.
    DD_WIN_PROF_API bool SetRumSession(const RumSessionContext* pContext);

    // Set volatile RUM view context with explicit view_id. Safe to call from any thread.
    // Pass nullptr/empty view_id to clear the current view (signals "between views").
    // Returns false if no session is active.
    DD_WIN_PROF_API bool SetRumView(const RumViewValues* pContext);

    // Clear all RUM context (both session and view). Call on session end.
    // Completes any pending session and view records before clearing.
    DD_WIN_PROF_API void ClearRumContext();
}
