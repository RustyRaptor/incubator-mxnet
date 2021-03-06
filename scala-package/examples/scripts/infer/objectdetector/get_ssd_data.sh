#!/bin/bash

# Licensed to the Apache Software Foundation (ASF) under one
# or more contributor license agreements.  See the NOTICE file
# distributed with this work for additional information
# regarding copyright ownership.  The ASF licenses this file
# to you under the Apache License, Version 2.0 (the
# "License"); you may not use this file except in compliance
# with the License.  You may obtain a copy of the License at
#
#   http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing,
# software distributed under the License is distributed on an
# "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
# KIND, either express or implied.  See the License for the
# specific language governing permissions and limitations
# under the License.


set -e

MXNET_ROOT=$(cd "$(dirname $0)/../../../.."; pwd)

data_path=$MXNET_ROOT/examples/scripts/infer/models/resnet50_ssd

image_path=$MXNET_ROOT/examples/scripts/infer/images

if [ ! -d "$data_path" ]; then
  mkdir -p "$data_path"
fi

if [ ! -d "$image_path" ]; then
  mkdir -p "$image_path"
fi

if [ ! -f "$data_path" ]; then
    curl https://s3.amazonaws.com/model-server/models/resnet50_ssd/resnet50_ssd_model-symbol.json -o $data_path/resnet50_ssd_model-symbol.json
    curl https://s3.amazonaws.com/model-server/models/resnet50_ssd/resnet50_ssd_model-0000.params -o $data_path/resnet50_ssd_model-0000.params
    curl https://s3.amazonaws.com/model-server/models/resnet50_ssd/synset.txt -o $data_path/synset.txt
    curl https://cloud.githubusercontent.com/assets/3307514/20012566/cbb53c76-a27d-11e6-9aaa-91939c9a1cd5.jpg -o $image_path/000001.jpg
    curl https://cloud.githubusercontent.com/assets/3307514/20012567/cbb60336-a27d-11e6-93ff-cbc3f09f5c9e.jpg -o $image_path/dog.jpg
    curl https://cloud.githubusercontent.com/assets/3307514/20012563/cbb41382-a27d-11e6-92a9-18dab4fd1ad3.jpg -o $image_path/person.jpg
fi

