#pragma once

#include <string>
#include <system_error>

namespace l4{

    [[noreturn]] inline void throw_system_error(
        const std::string& operation,
        int errorcode
    ){
        throw std::system_error(
            errorcode,
            std::generic_category(),
            operation
        );
    }

}