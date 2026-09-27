#include <atomic>
#include <iostream>
#include <thread>
#include <type_traits>

int main()
{
    std::atomic< std::thread::id > a{ std::thread::id{} };
    std::cout << "sizeof(std::thread::id): " << sizeof( std::thread::id ) << " bytes\n";
    std::cout << "is_lock_free: " << a.is_lock_free() << "\n";
    std::cout << "is_trivially_copyable: " << std::is_trivially_copyable_v< std::thread::id > << "\n";

    // also check pointer and a plain int for comparison
    std::atomic< void* > ptrAtomic{ nullptr };
    std::cout << "atomic<void*> is_lock_free: " << ptrAtomic.is_lock_free() << "\n";

    std::atomic< int > intAtomic{ 0 };
    std::cout << "atomic<int> is_lock_free: " << intAtomic.is_lock_free() << "\n";
}
