script_folder="/Applications/dev/cplusplus/DBWaller/test_package/build/apple-clang-17-armv8-20-release/generators"
echo "echo Restoring environment" > "$script_folder/deactivate_conanbuildenv-release-armv8.sh"
for v in 
do
    is_defined="true"
    value=$(printenv $v) || is_defined="" || true
    if [ -n "$value" ] || [ -n "$is_defined" ]
    then
        echo export "$v='$value'" >> "$script_folder/deactivate_conanbuildenv-release-armv8.sh"
    else
        echo unset $v >> "$script_folder/deactivate_conanbuildenv-release-armv8.sh"
    fi
done

