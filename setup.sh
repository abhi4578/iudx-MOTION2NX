#!/bin/bash 

variable_name=BASE_DIR
variable_value=$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )
echo "export "$variable_name"="$variable_value>>~/.bashrc
echo $variable_name"="$variable_value>>~/.profile
source ~/.bashrc
source ~/.profile
