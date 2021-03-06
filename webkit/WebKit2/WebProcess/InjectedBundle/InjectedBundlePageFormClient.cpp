/*
 * Copyright (C) 2010 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. AND ITS CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL APPLE INC. OR ITS CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "InjectedBundlePageFormClient.h"

#include "ImmutableDictionary.h"
#include "InjectedBundleNodeHandle.h"
#include "WKAPICast.h"
#include "WKBundleAPICast.h"
#include <WebCore/HTMLFormElement.h>
#include <WebCore/HTMLInputElement.h>
#include <WebCore/HTMLTextAreaElement.h>

using namespace WebCore;

namespace WebKit {

InjectedBundlePageFormClient::InjectedBundlePageFormClient()
{
    initialize(0);
}

void InjectedBundlePageFormClient::initialize(WKBundlePageFormClient* client)
{
    if (client && !client->version)
        m_client = *client;
    else 
        memset(&m_client, 0, sizeof(m_client));
}

void InjectedBundlePageFormClient::textFieldDidBeginEditing(WebPage* page, HTMLInputElement* inputElement, WebFrame* frame)
{
    if (!m_client.textFieldDidBeginEditing)
        return;

    RefPtr<InjectedBundleNodeHandle> nodeHandle = InjectedBundleNodeHandle::getOrCreate(inputElement);
    m_client.textFieldDidBeginEditing(toRef(page), toRef(nodeHandle.get()), toRef(frame), m_client.clientInfo);
}

void InjectedBundlePageFormClient::textFieldDidEndEditing(WebPage* page, HTMLInputElement* inputElement, WebFrame* frame)
{
    if (!m_client.textFieldDidEndEditing)
        return;

    RefPtr<InjectedBundleNodeHandle> nodeHandle = InjectedBundleNodeHandle::getOrCreate(inputElement);
    m_client.textFieldDidEndEditing(toRef(page), toRef(nodeHandle.get()), toRef(frame), m_client.clientInfo);
}

void InjectedBundlePageFormClient::textDidChangeInTextField(WebPage* page, HTMLInputElement* inputElement, WebFrame* frame)
{
    if (!m_client.textDidChangeInTextField)
        return;

    RefPtr<InjectedBundleNodeHandle> nodeHandle = InjectedBundleNodeHandle::getOrCreate(inputElement);
    m_client.textDidChangeInTextField(toRef(page), toRef(nodeHandle.get()), toRef(frame), m_client.clientInfo);
}

void InjectedBundlePageFormClient::textDidChangeInTextArea(WebPage* page, HTMLTextAreaElement* textAreaElement, WebFrame* frame)
{
    if (!m_client.textDidChangeInTextArea)
        return;

    RefPtr<InjectedBundleNodeHandle> nodeHandle = InjectedBundleNodeHandle::getOrCreate(textAreaElement);
    m_client.textDidChangeInTextArea(toRef(page), toRef(nodeHandle.get()), toRef(frame), m_client.clientInfo);
}

void InjectedBundlePageFormClient::willSubmitForm(WebPage* page, HTMLFormElement* formElement, WebFrame* frame, WebFrame* sourceFrame, const Vector<std::pair<String, String> >& values, RefPtr<APIObject>& userData)
{
    if (!m_client.willSubmitForm)
        return;

    RefPtr<InjectedBundleNodeHandle> nodeHandle = InjectedBundleNodeHandle::getOrCreate(formElement);

    ImmutableDictionary::MapType map;
    for (size_t i = 0; i < values.size(); ++i)
        map.set(values[i].first, WebString::create(values[i].second));
    RefPtr<ImmutableDictionary> textFieldsMap = ImmutableDictionary::adopt(map);

    WKTypeRef userDataToPass = 0;
    m_client.willSubmitForm(toRef(page), toRef(nodeHandle.get()), toRef(frame), toRef(sourceFrame), toRef(textFieldsMap.get()), &userDataToPass, m_client.clientInfo);
    userData = adoptRef(toWK(userDataToPass));
}

} // namespace WebKit
