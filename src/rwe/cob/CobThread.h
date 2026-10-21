#pragma once

#include <rwe/cob/CobFunction.h>
#include <stack>
#include <string>
#include <vector>

namespace rwe
{
    // Read-only stack iteration avoids copying/allocating every COB frame while
    // hashing. Execution retains the normal std::stack interface and ordering.
    template <typename T>
    class CobStack : public std::stack<T>
    {
    public:
        const auto& elements() const { return this->c; }
    };

    class CobThread
    {
    public:
        std::string name;

        CobStack<int> stack;

        unsigned int signalMask{0};

        CobStack<CobFunction> callStack;

        int returnValue{0};

        /**
         * Required for query functions, which communicate back to the engine
         * not by a return value but by changing the values of their input parameters.
         */
        std::vector<int> returnLocals;

    public:
        CobThread(const std::string& name, unsigned int signalMask);

        explicit CobThread(const std::string& name);
    };
}
