// SPDX-License-Identifier: Apache-2.0
// Copyright © 2026 Norvitex OÜ
//
// vSphere task polling helper

#include "TaskWait.hpp"

#include <algorithm>
#include <chrono>
#include <thread>

#include "vsphere-soap/Error.hpp"

namespace VSphereSoap::Detail {

using VSphereSoap::Error;
using VSphereSoap::ErrorCode;

Result<MoRef> waitForVimTask(VimSession& session, const MoRef& task)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(10);
    auto backoff = std::chrono::milliseconds(250);
    constexpr auto kMaxBackoff = std::chrono::milliseconds(5000);

    for (;;) {
        pugi::xml_document request = makeVimRequest("RetrievePropertiesEx", session.propertyCollector());
        pugi::xml_node op = request.document_element();
        pugi::xml_node specSet = op.append_child("specSet");
        pugi::xml_node propSet = specSet.append_child("propSet");
        propSet.append_child("type").text() = "Task";
        propSet.append_child("all").text() = "false";
        propSet.append_child("pathSet").text() = "info.state";
        propSet.append_child("pathSet").text() = "info.result";
        propSet.append_child("pathSet").text() = "info.error";
        pugi::xml_node objectSet = specSet.append_child("objectSet");
        pugi::xml_node obj = objectSet.append_child("obj");
        obj.append_attribute("type") = task.type.c_str();
        obj.text() = task.value.c_str();
        op.append_child("options");

        auto callResult = session.call(op);
        if (!callResult) {
            return std::move(callResult).error();
        }

        const pugi::xml_node objectContent = returnval(soapBodyContent(callResult.value())).child("objects");

        std::string state;
        MoRef taskResult;
        std::string errorMessage;
        for (pugi::xml_node prop = objectContent.child("propSet"); prop; prop = prop.next_sibling("propSet")) {
            const std::string name = prop.child("name").text().as_string();
            const pugi::xml_node val = prop.child("val");
            if (name == "info.state") {
                state = val.text().as_string();
            } else if (name == "info.result") {
                taskResult = parseMoRef(val);
            } else if (name == "info.error") {
                if (const pugi::xml_node msg = val.child("localizedMessage")) {
                    errorMessage = msg.text().as_string();
                }
            }
        }

        if (state == "success") {
            return taskResult;
        }
        if (state == "error") {
            return Error(ErrorCode::IoError, "vSphere task " + task.value +
                                                  " failed: " + (errorMessage.empty() ? "unknown error" : errorMessage));
        }

        if (std::chrono::steady_clock::now() >= deadline) {
            return Error(ErrorCode::IoError, "vSphere task " + task.value + " timed out waiting for completion");
        }
        std::this_thread::sleep_for(backoff);
        backoff = std::min(backoff * 2, kMaxBackoff);
    }
}

} // namespace VSphereSoap::Detail
