/*
 * Copyright 2026 Google Inc.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#pragma once

// Runs the randomized curve/pixel property tests.  This is declared separately
// so tests.c stays focused on the hand-written regression tests.
void skcms_RunRandomPropertyTests(void);
