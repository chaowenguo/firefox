/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "ExternalTexture.h"

#include "mozilla/dom/WebGPUBinding.h"

namespace mozilla::webgpu {

GPU_IMPL_CYCLE_COLLECTION(ExternalTexture, mGlobal)

JSObject* ExternalTexture::WrapObject(JSContext* cx,
                                      JS ::Handle<JSObject*> givenProto) {
  return dom::GPUExternalTexture_Binding::Wrap(cx, this, givenProto);
}

}  // namespace mozilla::webgpu
