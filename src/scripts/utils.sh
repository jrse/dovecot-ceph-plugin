#!/bin/bash

# This script includes several utility functions.

set -o errexit
#set -o nounset

# globals

# Lists of excluded files and directories
declare -a excluded_files=("dovecot-acl-list" "subscriptions")
declare -a excluded_file_wildcards=("dovecot-uidvalidity")
declare -a excluded_directories=("storage" "alt-storage" "dbox-Mails" "rbox-Mails")

prog_name=$(basename "$0")

# functions

# An error exit function
#
# Parameter $1 error message
# 
error_exit()
{
	echo "${prog_name}: Error: ${1:-"Unknown Error"}" 1>&2
	exit 1
}

# Tests if string1 contains string2
#
# Parameter $1 string
# Parameter $2 substring
#
string_contains() {
	[ -z "${1##*$2*}" ]; 
}

# Returns true if the exclude lists contain the given name
# else false
#
# Parameter $1 the name we are looking for
#	
is_excluded() {
	local found=0
	local file="$1"
	
	for excluded_file in "${excluded_files[@]}"; do
		if [ ${#file} -eq ${#excluded_file} ]; then
			if [[ "$file" = "$excluded_file" ]]; then
				found=1
				break
			fi
		fi	
	done
	
	if [ $found -eq 0 ]; then
		for excluded_wildcard in "${excluded_file_wildcards[@]}"; do
			if [ ${#file} -ge ${#excluded_wildcard} ]; then
				local temp_file=${file:0:${#excluded_wildcard}}
				if [[ "$temp_file" = "$excluded_wildcard" ]]; then
					found=1
					break
				fi
			fi	
		done
	fi
	
	if [ $found -eq 0 ]; then
		for excluded_directory in "${excluded_directories[@]}"; do
			if [ ${#file} -eq ${#excluded_directory} ]; then
				if [[ "$file" = "$excluded_directory" ]]; then
					found=1
					break
				fi
			fi	
		done
	fi
	
	if [ $found -eq 1 ]; then
		true
	else 
		false
	fi
}

# Copies files from one directory to another one
#
root_path=

copy_files() {
	local src_path=$1
	local dest_path="$2"
	local output=
	
	#excluded_directories[3]=$4
	
	if [ -z "$root_path" ]; then
		root_path=$src_path
	fi
												
	for file in "$src_path"/*; do 
		output=${file/$root_path/$dest_path\/$3}
		output=${output//\/\//\/}
		if [ -f "$file" ]; then
			if ! is_excluded "${file##*/}"; then
				#echo "mkdir ${output%/*}"
				mkdir -p "${output%/*}" || error_exit "could not create directory ${output%/*}"
				#echo "cp: $file -> $output"
				cp "$file" "$output" || error_exit "copy $file to $output failed"
			fi
		fi	
		if [ -d "$file" ]; then
			if ! is_excluded "${file##*/}"; then
				copy_files "$file" "$dest_path" "$3" || exit 1
			fi
		fi
	done
}
