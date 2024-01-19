/**
 *    Copyright (C) 2022-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/db/auth/validated_tenancy_scope.h"

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <string>

#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/db/auth/validated_tenancy_scope_gen.h"
#include "mongo/db/operation_context.h"
#include "mongo/util/decorable.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kAccessControl

namespace mongo::auth {
namespace {
using namespace fmt::literals;

const auto validatedTenancyScopeDecoration =
    OperationContext::declareDecoration<boost::optional<ValidatedTenancyScope>>();

}  // namespace

bool ValidatedTenancyScope::hasAuthenticatedUser() const {
    return holds_alternative<UserName>(_tenantOrUser);
}

const UserName& ValidatedTenancyScope::authenticatedUser() const {
    invariant(hasAuthenticatedUser());
    return std::get<UserName>(_tenantOrUser);
}

const boost::optional<ValidatedTenancyScope>& ValidatedTenancyScope::get(OperationContext* opCtx) {
    return validatedTenancyScopeDecoration(opCtx);
}

void ValidatedTenancyScope::set(OperationContext* opCtx,
                                boost::optional<ValidatedTenancyScope> token) {
    validatedTenancyScopeDecoration(opCtx) = std::move(token);
}

}  // namespace mongo::auth