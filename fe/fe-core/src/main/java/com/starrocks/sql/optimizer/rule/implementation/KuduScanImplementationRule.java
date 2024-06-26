// Copyright 2021-present StarRocks, Inc. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

package com.starrocks.sql.optimizer.rule.implementation;

import com.google.common.collect.Lists;
import com.starrocks.sql.optimizer.OptExpression;
import com.starrocks.sql.optimizer.OptimizerContext;
import com.starrocks.sql.optimizer.operator.OperatorType;
import com.starrocks.sql.optimizer.operator.logical.LogicalKuduScanOperator;
import com.starrocks.sql.optimizer.operator.pattern.Pattern;
import com.starrocks.sql.optimizer.operator.physical.PhysicalKuduScanOperator;
import com.starrocks.sql.optimizer.rule.RuleType;

import java.util.List;

public class KuduScanImplementationRule extends ImplementationRule {
    public KuduScanImplementationRule() {
        super(RuleType.IMP_KUDU_LSCAN_TO_PSCAN,
                Pattern.create(OperatorType.LOGICAL_KUDU_SCAN));
    }

    @Override
    public List<OptExpression> transform(OptExpression input, OptimizerContext context) {
        LogicalKuduScanOperator scan = (LogicalKuduScanOperator) input.getOp();
        PhysicalKuduScanOperator physicalKuduScan = new PhysicalKuduScanOperator(scan);
        OptExpression result = new OptExpression(physicalKuduScan);
        return Lists.newArrayList(result);
    }
}
