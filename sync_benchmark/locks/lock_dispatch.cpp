/**
 * locks/lock_dispatch.cpp
 *
 * Provides the out-of-line definitions for thread_local static members
 * of MCSLockWrapper and CLHLockWrapper.
 *
 * Required in C++14 because "inline" on a static thread_local variable
 * is a C++17 extension. By putting the definitions here we support both
 * -std=c++14 and -std=c++17.
 *
 * NOTE: This file must be compiled and linked with all other .cpp files.
 * It is already included in the Makefile and CMakeLists.txt.
 */
#include "lock_dispatch.h"

thread_local MCSNode  MCSLockWrapper::tl_node;
thread_local CLHNode* CLHLockWrapper::tl_node = new CLHNode();
thread_local CLHNode* CLHLockWrapper::tl_pred = nullptr;
