#!/bin/bash

# main

# Loads utils script
script_path=${0%/*}
echo "script_path = $script_path"
source $script_path/utils.sh

# Reads mail_location of user
mail_location=$(get_user_mail_location $1)

# Synchronize from mdbox to rbox
result=$(doveadm_sync $2 $1)
[ $result -eq 0 ] && echo "success" || echo "fail"

# Copies files from mdbox to rbox
copy_files $mail_location $2 $1 


