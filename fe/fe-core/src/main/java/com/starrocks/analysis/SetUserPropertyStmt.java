// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

package com.starrocks.analysis;

import com.google.common.collect.Lists;
import com.starrocks.common.Pair;
import com.starrocks.sql.ast.AstVisitor;

import java.util.List;

public class SetUserPropertyStmt extends DdlStmt {
    private String user;
    private final List<SetVar> propertyList;

    public SetUserPropertyStmt(String user, List<SetVar> propertyList) {
        this.user = user;
        this.propertyList = propertyList;
    }

    public String getUser() {
        return user;
    }

    public void setUser(String user) {
        this.user = user;
    }

    public List<SetVar> getPropertyList() {
        return this.propertyList;
    }

    // using List because we need retain the origin property order
    public List<Pair<String, String>> getPropertyPairList() {
        List<Pair<String, String>> list = Lists.newArrayList();
        for (SetVar var : propertyList) {
            list.add(Pair.create(((SetUserPropertyVar) var).getPropertyKey(),
                    ((SetUserPropertyVar) var).getPropertyValue()));
        }
        return list;
    }

    @Override
    public boolean isSupportNewPlanner() {
        return true;
    }

    @Override
    public <R, C> R accept(AstVisitor<R, C> visitor, C context) {
        return visitor.visitSetUserPropertyStmt(this, context);
    }

    @Override
    public String toSql() {
        StringBuilder sb = new StringBuilder();
        sb.append("SET PROPERTY FOR '");
        sb.append(user);
        sb.append("' ");

        int idx = 0;
        for (SetVar var : propertyList) {
            if (idx != 0) {
                sb.append(", ");
            }
            sb.append(var.toSql());
            idx++;
        }
        return sb.toString();
    }

    @Override
    public String toString() {
        return toSql();
    }
}

