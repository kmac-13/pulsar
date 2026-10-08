#pragma once
#ifndef KMAC_STELLYRA_CALLABLE_H
#define KMAC_STELLYRA_CALLABLE_H

/**
 * @file callable.h
 * @brief Callable<ReturnType(Args...)> - move-only, type-erased callable
 * used throughout Stellyra, avoiding std::function use.
 *
 * Every HandlerEntry stores one Callable; every connect()-family overload
 * ultimately produces one via create()/createPartial().  A Callable is a
 * flat (void* _obj, stub function pointer, optional destructor function
 * pointer) triple - no virtual dispatch, no heap allocation except for the
 * owning-functor form (create(F&&)).  See the class documentation below for
 * the three construction forms and their ownership rules.
 */

#include <cassert>
#include <tuple>
#include <type_traits>
#include <utility>

namespace kmac {
namespace stellyra {

/**
 * @brief Primary template, intentionally left undefined - only the
 * ReturnType(Args...) specialization below is ever usable.  Exists so that
 * Callable<ReturnType(Args...)> reads as an ordinary function-signature
 * template parameter at call sites, matching std::function's spelling.
 */
template< typename T >
class Callable;

/**
 * @brief Lightweight type-erased callable: wraps a free function, a
 * pointer-to-member-function bound to an object, or a functor (capturing or
 * non-capturing lambda / callable object).
 *
 * Callable is MOVE-ONLY.  This is a deliberate, unconditional property of
 * the type - not dependent on which factory created a given instance.
 * Function-pointer and method-pointer Callables are trivially copyable in
 * principle, but Callable does not special-case this: uniform move-only
 * semantics mean every Callable can be treated identically regardless of
 * how it was constructed, and a Callable holding a heap-owned functor (see
 * create(F&&) below) can never be accidentally copied (which would either
 * double-free or require a deep-copy hook this class deliberately does not
 * provide).  Move the Callable (e.g. via std::move into a std::unique_ptr's
 * pointee, or as a class member during single-construction setup) wherever
 * a copy would otherwise be written.
 *
 * Three construction forms:
 *   - create<Func>()          - free function / non-capturing lambda
 *                               decayed to a function pointer (default
 *                               overload resolution: a non-capturing
 *                               lambda implicitly converts to its
 *                               function-pointer type, so Func is that
 *                               converted pointer's target)
 *   - create<T, Method>(obj)  - pointer-to-member-function Method bound
 *                               to *obj; non-owning, so obj must outlive
 *                               the Callable
 *   - create(F&& functor)     - any callable object F (typically a
 *                               capturing lambda); the functor is
 *                               heap-allocated via `new` and owned by
 *                               the Callable for its lifetime (or until
 *                               moved-from); freed via `delete` in the
 *                               destructor / on move-assignment overwrite
 *
 * A default-constructed Callable is "empty": operator bool() returns false,
 * and operator() asserts.  Use operator bool() (or operator!()) to check
 * validity before calling.
 */
template< typename ReturnType, typename... Args >
class Callable< ReturnType ( Args... ) >
{
private:
	using this_type = Callable< ReturnType ( Args... ) >;
	using stub_func_type = ReturnType (*)( void*, Args... );  ///< invokes the wrapped target, given _obj
	using dt_func_type = void (*)( void* );  ///< frees a heap-owned functor; null unless create(F&&) was used

	/**
	 * @brief Invokes a free function captured at compile time as the NTTP
	 * Func.  obj is unused (nothing to bind for a free function).
	 */
	template< ReturnType (*Func)( Args... ) >
	static ReturnType functionStub( void* obj, Args... args );

	/**
	 * @brief Invokes a free function pointer captured at runtime (not as a
	 * compile-time NTTP) and stored in _obj via reinterpret_cast.
	 *
	 * Converting a function pointer through void* and back is
	 * implementation-defined per the standard (function pointers and
	 * object pointers aren't guaranteed interconvertible), but is
	 * well-defined in practice on every mainstream target this library
	 * supports - POSIX requires it for dlsym, it holds on Windows, and it
	 * holds on von Neumann embedded targets (e.g. ARM Cortex-M/FreeRTOS).
	 * A genuine Harvard-architecture target with disjoint code/data
	 * address spaces would need a different stub.
	 */
	static ReturnType runtimeFunctionStub( void* obj, Args... args );

	/**
	 * @brief Invokes Method on *obj.  ClassType comes from
	 * MethodTraits<decltype(Method)>::ClassType (the class Method is
	 * actually declared on) rather than being separately supplied, so a
	 * pointer to any class derived from ClassType converts implicitly -
	 * connecting an inherited-but-not-redeclared method through a
	 * subclass works the same way it already does for partialMethodStub.
	 */
	template< auto Method >
	static ReturnType methodStub( void* obj, Args... args );

	/**
	 * @brief Invokes an arbitrary functor (capturing or non-capturing lambda, or
	 * any callable object) stored at `obj`.
	 */
	template< typename F >
	static ReturnType functorStub( void* obj, Args... args );

	/**
	 * @brief Deletes a heap-allocated functor created by create(F&&). Only ever
	 * installed (non-null _destructor) for the create(F&&) form.
	 */
	template< typename F >
	static void functorDestructorStub( void* obj );

	// ------------------------------------------------------------------
	// Partial argument matching (createPartial, below)
	//
	// Extracts ClassType/HandlerArgs from a pointer-to-member-function
	// type via partial specialization. Only the primary template is
	// declared here; MethodTraits is specialized (for ReturnType(T::*)
	// (HandlerArgs...)) in the implementation section.
	// ------------------------------------------------------------------

	template< typename MethodPtr >
	struct MethodTraits;

	// Free-function counterpart of MethodTraits: extracts HandlerArgs from
	// a HandlerReturn(*)(HandlerArgs...) function pointer type. Specialized
	// in the implementation section.
	template< typename FuncPtr >
	struct FuncTraits;

	/**
	 * @brief Forwards the first sizeof...(HandlerArgs) of Args... to *obj.*Method,
	 * dropping the rest.  HandlerArgs/ClassType come from
	 * MethodTraits<decltype(Method)>.
	 */
	template< auto Method >
	static ReturnType partialMethodStub( void* obj, Args... args );

	/**
	 * @brief Free-function counterpart of partialMethodStub: forwards the first
	 * sizeof...(HandlerArgs) of Args... to Func, dropping the rest.
	 * HandlerArgs come from FuncTraits<decltype(Func)>.  obj is unused
	 * (always nullptr for free functions - nothing to bind).
	 */
	template< auto Func >
	static ReturnType partialFunctionStub( void* obj, Args... args );

	/**
	 * @brief Index-sequence helper for partialMethodStub: unpacks argTuple's first
	 * sizeof...(I) elements as the call to *obj.*Method.  argTuple is
	 * std::tuple<Args&&...> as produced by std::forward_as_tuple - a tuple
	 * of references into partialMethodStub's parameter pack, not a tuple of
	 * values.
	 */
	template< typename T, auto Method, std::size_t... I >
	static ReturnType invokePartial( T* obj, std::tuple< Args&&... > argTuple, std::index_sequence< I... > );

	/**
	 * @brief Free-function counterpart of invokePartial.
	 */
	template< auto Func, std::size_t... I >
	static ReturnType invokePartialFree( std::tuple< Args&&... > argTuple, std::index_sequence< I... > );

public:
	/**
	 * @brief Wrap a free function (or non-capturing lambda implicitly
	 * converted to a function pointer of this type).
	 *
	 * Non-owning: Func's address is a compile-time constant, nothing to
	 * own.
	 */
	template< ReturnType (*Func)( Args... ) >
	static this_type create();

	/**
	 * @brief Wrap a free function pointer captured at runtime (e.g. passed
	 * as a normal argument, as opposed to create<Func>()'s compile-time
	 * NTTP form).
	 *
	 * Non-owning, no allocation - same cost class as create<Func>(), with
	 * one extra indirection (the function's address is read from _obj at
	 * call time instead of being baked into the stub at compile time).
	 */
	static this_type create( ReturnType (*func)( Args... ) );

	/**
	 * @brief Wrap a pointer-to-member-function bound to *obj.
	 *
	 * Non-owning: obj must outlive the Callable.  obj's parameter type is
	 * MethodTraits<decltype(Method)>::ClassType (the class Method is
	 * actually declared on), not a separately-supplied T, so passing a
	 * pointer to any subclass works via ordinary implicit upcast - the
	 * method does not need to be redeclared on the subclass.
	 */
	template< auto Method >
	static this_type create( typename MethodTraits< decltype( Method ) >::ClassType* obj );

	/**
	 * @brief Wrap a pointer-to-member-function bound to *obj, accepting
	 * FEWER arguments than this Callable's Args... - the trailing Args
	 * are silently dropped.
	 *
	 * Non-owning: obj must outlive the Callable (same as create<T,Method>(obj)).
	 *
	 * Method's parameter list (HandlerArgs...) is deduced from the single
	 * `Method` template argument via MethodTraits; only Method need be
	 * specified explicitly:
	 *
	 * @code
	 * // Callable<void(int,int,std::string*)>
	 * auto d = Callable<void(int,int,std::string*)>::createPartial<&Receiver::onFirstArg>(&receiver);
	 * // calls receiver.onFirstArg(firstArg), dropping the int and std::string* args
	 * @endcode
	 *
	 * A static_assert fires if Method requests more arguments than Args...
	 * provides.
	 */
	template< auto Method >
	static this_type createPartial( typename MethodTraits< decltype( Method ) >::ClassType* obj );

	/**
	 * @brief Wrap a free function (or non-capturing lambda implicitly
	 * converted to a function pointer), accepting FEWER arguments than
	 * this Callable's Args... - the trailing Args are silently dropped.
	 *
	 * Non-owning: Func's address is a compile-time constant, nothing to own.
	 *
	 * Func's parameter list (HandlerArgs...) is deduced from the single
	 * `Func` template argument via FuncTraits; only Func need be specified
	 * explicitly:
	 *
	 * @code
	 * // Callable<void(int,int,std::string*)>
	 * void onFirstArg(int x) { ... }
	 * auto d = Callable<void(int,int,std::string*)>::createPartial<&onFirstArg>();
	 * // calls onFirstArg(firstArg), dropping the int and std::string* args
	 * @endcode
	 *
	 * Distinguished from createPartial<Method>(obj) (above) by argument
	 * count: this overload takes none.
	 *
	 * A static_assert fires if Func requests more arguments than Args...
	 * provides.
	 */
	template< auto Func >
	static this_type createPartial();

	/**
	 * @brief Wrap a functor (typically a capturing lambda) by heap-allocated
	 * copy/move.
	 *
	 * Owning: a decayed copy of `functor` is allocated via `new` and owned
	 * by the returned Callable (freed via `delete` in ~Callable() or when
	 * overwritten by move-assignment).  The original `functor` (e.g. a
	 * temporary lambda at the call site) need not outlive this call.
	 */
	template< typename F >
	static this_type create( F&& functor );

private:
	void* _obj;                ///< bound object / heap-owned functor / reinterpret_cast'd function pointer; null for NTTP free-function form
	stub_func_type _callback;  ///< dispatches to the right stub for however this Callable was constructed; null means empty
	dt_func_type _destructor;  // non-null only for the create(F&&) owning form

	/**
	 * @brief Raw constructor used by every create()/createPartial() factory
	 * to assemble the (obj, callback, destructor) triple directly.  Not
	 * exposed publicly - callers go through the named factories so the
	 * triple is always internally consistent (e.g. a non-null destructor
	 * only ever paired with a heap-owned obj).
	 */
	Callable( void* obj, stub_func_type callback, dt_func_type destructor );

public:
	/**
	 * @brief Constructs an empty Callable: operator bool() is false,
	 * operator() asserts.
	 */
	Callable();
	~Callable();

	/**
	 * @brief Move-only - see class documentation.  Copy is intentionally absent
	 * (not =deleted as a special case; simply never declared), so any
	 * accidental copy is a compile error at the call site.
	 */
	Callable( const Callable& ) = delete;
	Callable& operator=( const Callable& ) = delete;

	Callable( Callable&& other ) noexcept;
	Callable& operator=( Callable&& other ) noexcept;

	/**
	 * @brief Returns true if this Callable is bound to an owning object
	 * (i.e. was created via create<T,Method>(obj) or create(obj)).
	 *
	 * Returns false for free functions, non-capturing lambdas, and empty
	 * Callables.  Does not expose the object's address.
	 */
	bool hasOwner() const;

	/**
	 * @brief Returns true if this Callable's owning target is `obj`
	 * (i.e. it was created via create<T,Method>(obj) with this exact pointer).
	 * Always false for empty, function-pointer, or owning-functor Callables.
	 */
	bool isOwner( void* obj ) const;

	/**
	 * @brief If this Callable holds an owning functor of exactly type Closure
	 * (the create(F&&) form), returns a typed pointer to it; otherwise returns
	 * nullptr.
	 *
	 * The guard compares _callback against &functorStub<Closure>, an address
	 * installed only by create(F&&) for that exact Closure type, so it matches
	 * that one functor type and nothing else - no RTTI, and no false positive
	 * against the method-pointer, free-function, or a differently-typed
	 * owning-functor form.  The returned pointer is therefore safe to
	 * dereference.  Reads back a stored functor's fields for identity
	 * comparison, e.g. a PmfInvoker's receiver and method in
	 * disconnect(receiver, method).
	 */
	template< typename Closure >
	const Closure* targetAs() const;

	/**
	 * @brief True if this Callable has a target (was not default-constructed and
	 * has not been moved-from).
	 */
	operator bool() const;
	bool operator!() const;

	/**
	 * @brief Equality compares (_obj, _callback) pairs only.
	 *
	 * For owning-functor Callables this is pointer-identity on the
	 * heap-allocated functor: two Callables that each own their own copy of an
	 * equivalent functor compare unequal. Value-equality across independently
	 * constructed owning functors is intentionally not attempted here; when a
	 * specific owning functor's contents must be matched (as in
	 * disconnect(receiver, method) matching a stored PmfInvoker), the match is
	 * done by reading the functor's fields through targetAs, not through this
	 * operator.
	 */
	bool operator==( const Callable& other ) const;
	bool operator!=( const Callable& other ) const;

	/**
	 * @brief Invoke the wrapped callable.
	 *
	 * Asserts (via assert(), compiled out under NDEBUG like the rest of
	 * this codebase) if this Callable is empty.
	 */
	ReturnType operator()( Args... args ) const;

private:
	/**
	 * @brief Frees the owned functor (if any) and resets to the empty state.
	 */
	void clear();
};


// ============================================================================
// Implementation
// ============================================================================

template< typename ReturnType, typename... Args >
template< ReturnType (*Func)( Args... ) >
inline ReturnType Callable< ReturnType ( Args... ) >::functionStub( void* /*obj*/, Args... args )
{
	return Func( std::forward< Args >( args )... );
}

template< typename ReturnType, typename... Args >
inline ReturnType Callable< ReturnType ( Args... ) >::runtimeFunctionStub( void* obj, Args... args )
{
	using FuncPtr = ReturnType (*)( Args... );
	FuncPtr func = reinterpret_cast< FuncPtr >( obj );
	return func( std::forward< Args >( args )... );
}

template< typename ReturnType, typename... Args >
template< auto Method >
inline ReturnType Callable< ReturnType ( Args... ) >::methodStub( void* obj, Args... args )
{
	using ClassType = typename MethodTraits< decltype( Method ) >::ClassType;
	return (static_cast< ClassType* >( obj )->*Method)( std::forward< Args >( args )... );
}

template< typename ReturnType, typename... Args >
template< typename F >
inline ReturnType Callable< ReturnType ( Args... ) >::functorStub( void* obj, Args... args )
{
	return (*static_cast< F* >( obj ))( std::forward< Args >( args )... );
}

template< typename ReturnType, typename... Args >
template< typename F >
inline void Callable< ReturnType ( Args... ) >::functorDestructorStub( void* obj )
{
	delete static_cast< F* >( obj );
}

template< typename ReturnType, typename... Args >
template< ReturnType (*Func)( Args... ) >
inline typename Callable< ReturnType ( Args... ) >::this_type Callable< ReturnType ( Args... ) >::create()
{
	return this_type( nullptr, &functionStub< Func >, nullptr );
}

template< typename ReturnType, typename... Args >
inline typename Callable< ReturnType ( Args... ) >::this_type Callable< ReturnType ( Args... ) >::create( ReturnType (*func)( Args... ) )
{
	return this_type( reinterpret_cast< void* >( func ), &runtimeFunctionStub, nullptr );
}

template< typename ReturnType, typename... Args >
template< auto Method >
inline typename Callable< ReturnType ( Args... ) >::this_type Callable< ReturnType ( Args... ) >::create(
	typename MethodTraits< decltype( Method ) >::ClassType* obj )
{
	return this_type( static_cast< void* >( obj ), &methodStub< Method >, nullptr );
}

template< typename ReturnType, typename... Args >
template< typename F >
inline typename Callable< ReturnType ( Args... ) >::this_type Callable< ReturnType ( Args... ) >::create( F&& functor )
{
	using DecayedF = typename std::decay< F >::type;

	DecayedF* allocated = new DecayedF( std::forward< F >( functor ) );

	return this_type(
		static_cast< void* >( allocated ),
		&functorStub< DecayedF >,
		&functorDestructorStub< DecayedF > );
}

// ----------------------------------------------------------------------
// MethodTraits partial specialization: extracts ClassType and the
// (shorter) HandlerArgs... pack from a pointer-to-member-function type.
// Declared at namespace scope (member-template partial specialization),
// specialized for the Callable<ReturnType(Args...)> that's being
// constructed - HandlerReturn need not equal Callable's ReturnType, though
// in practice both are void for the event-handler use case.
// ----------------------------------------------------------------------
template< typename ReturnType, typename... Args >
template< typename T, typename HandlerReturn, typename... HandlerArgs >
struct Callable< ReturnType ( Args... ) >::MethodTraits< HandlerReturn ( T::* )( HandlerArgs... ) >
{
	using ClassType = T;
	using ArgsTuple = std::tuple< HandlerArgs... >;
	static constexpr std::size_t arity = sizeof...( HandlerArgs );
};

// noexcept is part of the function type since C++17, so a noexcept method
// pointer does not match the specialization above on its own - this one
// covers it by simply delegating to it (identical members either way)
template< typename ReturnType, typename... Args >
template< typename T, typename HandlerReturn, typename... HandlerArgs >
struct Callable< ReturnType ( Args... ) >::MethodTraits< HandlerReturn ( T::* )( HandlerArgs... ) noexcept >
	: Callable< ReturnType ( Args... ) >::template MethodTraits< HandlerReturn ( T::* )( HandlerArgs... ) >
{
};

template< typename ReturnType, typename... Args >
template< typename T, auto Method, std::size_t... I >
inline ReturnType Callable< ReturnType ( Args... ) >::invokePartial( T* obj, std::tuple< Args&&... > argTuple, std::index_sequence< I... > )
{
	(void) argTuple;  // unused when sizeof...(I) == 0 (Method takes no arguments)
	return (obj->*Method)( std::get< I >( std::move( argTuple ) )... );
}

template< typename ReturnType, typename... Args >
template< auto Method >
inline ReturnType Callable< ReturnType ( Args... ) >::partialMethodStub( void* obj, Args... args )
{
	using MethodPtr = decltype( Method );
	using Traits = MethodTraits< MethodPtr >;
	using T = typename Traits::ClassType;

	static_assert( Traits::arity <= sizeof...( Args ),
		"createPartial: Method requests more arguments than this Callable's Args... provides" );

	if constexpr ( Traits::arity == sizeof...( Args ) )
	{
		// full-arity: call directly, no tuple construction or index_sequence needed
		return ( static_cast< T* >( obj )->*Method )( std::forward< Args >( args )... );
	}
	else
	{
		return invokePartial< T, Method >(
			static_cast< T* >( obj ),
			std::forward_as_tuple( std::forward< Args >( args )... ),
			std::make_index_sequence< Traits::arity >{} );
	}
}

template< typename ReturnType, typename... Args >
template< auto Method >
inline typename Callable< ReturnType ( Args... ) >::this_type Callable< ReturnType ( Args... ) >::createPartial( typename MethodTraits< decltype( Method ) >::ClassType* obj )
{
	return this_type( static_cast< void* >( obj ), &partialMethodStub< Method >, nullptr );
}

// ----------------------------------------------------------------------
// FuncTraits partial specialization: free-function counterpart of
// MethodTraits, extracting the (shorter) HandlerArgs... pack from a
// HandlerReturn(*)(HandlerArgs...) function pointer type.
// ----------------------------------------------------------------------
template< typename ReturnType, typename... Args >
template< typename HandlerReturn, typename... HandlerArgs >
struct Callable< ReturnType ( Args... ) >::FuncTraits< HandlerReturn (*)( HandlerArgs... ) >
{
	using ArgsTuple = std::tuple< HandlerArgs... >;
	static constexpr std::size_t arity = sizeof...( HandlerArgs );
};

template< typename ReturnType, typename... Args >
template< auto Func, std::size_t... I >
inline ReturnType Callable< ReturnType ( Args... ) >::invokePartialFree( std::tuple< Args&&... > argTuple, std::index_sequence< I... > )
{
	(void) argTuple;  // unused when sizeof...(I) == 0 (Func takes no arguments)
	return Func( std::get< I >( std::move( argTuple ) )... );
}

template< typename ReturnType, typename... Args >
template< auto Func >
inline ReturnType Callable< ReturnType ( Args... ) >::partialFunctionStub( void* /*obj*/, Args... args )
{
	using Traits = FuncTraits< decltype( Func ) >;

	static_assert( Traits::arity <= sizeof...( Args ),
		"createPartial: Func requests more arguments than this Callable's Args... provides" );

	if constexpr ( Traits::arity == sizeof...( Args ) )
	{
		// full-arity: call directly, no tuple construction or index_sequence needed
		return Func( std::forward< Args >( args )... );
	}
	else
	{
		return invokePartialFree< Func >(
			std::forward_as_tuple( std::forward< Args >( args )... ),
			std::make_index_sequence< Traits::arity >{} );
	}
}

template< typename ReturnType, typename... Args >
template< auto Func >
inline typename Callable< ReturnType ( Args... ) >::this_type Callable< ReturnType ( Args... ) >::createPartial()
{
	return this_type( nullptr, &partialFunctionStub< Func >, nullptr );
}

template< typename ReturnType, typename... Args >
inline Callable< ReturnType ( Args... ) >::Callable( void* obj, stub_func_type callback, dt_func_type destructor )
	: _obj( obj )
	, _callback( callback )
	, _destructor( destructor )
{
}

template< typename ReturnType, typename... Args >
inline Callable< ReturnType ( Args... ) >::Callable()
	: _obj( nullptr )
	, _callback( nullptr )
	, _destructor( nullptr )
{
}

template< typename ReturnType, typename... Args >
inline Callable< ReturnType ( Args... ) >::~Callable()
{
	clear();
}

template< typename ReturnType, typename... Args >
inline Callable< ReturnType ( Args... ) >::Callable( Callable&& other ) noexcept
	: _obj( other._obj )
	, _callback( other._callback )
	, _destructor( other._destructor )
{
	other._obj = nullptr;
	other._callback = nullptr;
	other._destructor = nullptr;
}

template< typename ReturnType, typename... Args >
inline Callable< ReturnType ( Args... ) >& Callable< ReturnType ( Args... ) >::operator=( Callable&& other ) noexcept
{
	if ( this != &other )
	{
		clear();
		_obj = other._obj;
		_callback = other._callback;
		_destructor = other._destructor;

		other._obj = nullptr;
		other._callback = nullptr;
		other._destructor = nullptr;
	}
	return *this;
}

template< typename ReturnType, typename... Args >
inline bool Callable< ReturnType ( Args... ) >::hasOwner() const
{
	return _obj != nullptr;
}

template< typename ReturnType, typename... Args >
inline bool Callable< ReturnType ( Args... ) >::isOwner( void* obj ) const
{
	return _obj != nullptr && _obj == obj;
}

template< typename ReturnType, typename... Args >
template< typename Closure >
inline const Closure* Callable< ReturnType ( Args... ) >::targetAs() const
{
	return _callback == &functorStub< Closure >
		? static_cast< const Closure* >( _obj )
		: nullptr;
}

template< typename ReturnType, typename... Args >
inline Callable< ReturnType ( Args... ) >::operator bool() const
{
	return _callback != nullptr;
}

template< typename ReturnType, typename... Args >
inline bool Callable< ReturnType ( Args... ) >::operator!() const
{
	return _callback == nullptr;
}

template< typename ReturnType, typename... Args >
inline bool Callable< ReturnType ( Args... ) >::operator==( const Callable& other ) const
{
	return _obj == other._obj && _callback == other._callback;
}

template< typename ReturnType, typename... Args >
inline bool Callable< ReturnType ( Args... ) >::operator!=( const Callable& other ) const
{
	return !( *this == other );
}

template< typename ReturnType, typename... Args >
inline ReturnType Callable< ReturnType ( Args... ) >::operator()( Args... args ) const
{
	assert( _callback != nullptr && "Callable::operator(): callback has not been defined" );
	return _callback( _obj, std::forward< Args >( args )... );
}

template< typename ReturnType, typename... Args >
inline void Callable< ReturnType ( Args... ) >::clear()
{
	if ( _destructor && _obj )
	{
		_destructor( _obj );
	}
	_obj = nullptr;
	_callback = nullptr;
	_destructor = nullptr;
}

} // namespace stellyra
} // namespace kmac

#endif // KMAC_STELLYRA_CALLABLE_H
