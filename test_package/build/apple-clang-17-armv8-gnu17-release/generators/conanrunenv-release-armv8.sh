script_folder="/Applications/dev/cplusplus/DBWaller/test_package/build/apple-clang-17-armv8-gnu17-release/generators"
echo "echo Restoring environment" > "$script_folder/deactivate_conanrunenv-release-armv8.sh"
for v in OPENSSL_MODULES
do
    is_defined="true"
    value=$(printenv $v) || is_defined="" || true
    if [ -n "$value" ] || [ -n "$is_defined" ]
    then
        echo export "$v='$value'" >> "$script_folder/deactivate_conanrunenv-release-armv8.sh"
    else
        echo unset $v >> "$script_folder/deactivate_conanrunenv-release-armv8.sh"
    fi
done


export OPENSSL_MODULES="/Users/ojobasi/.conan2/p/b/opensf05bd3a136f24/p/lib/ossl-modules"