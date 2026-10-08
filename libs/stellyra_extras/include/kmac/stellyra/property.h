#pragma once
#ifndef KMAC_STELLYRA_PROPERTY_H
#define KMAC_STELLYRA_PROPERTY_H

/**
 * @file property.h
 * @brief Reactive property types built on the refactored Stellyra API.
 *
 * Four types form a consistent family.  All expose the same read interface
 * (implicit cast, get(), changed event) so that VM binding wiring and
 * NDL-compiled code never need to know which variant they are reading.
 *
 *   Property<T>                  - readable and writable by anyone
 *   ReadOnlyProperty<Owner, T>   - readable externally, writable only by Owner
 *   ComputedProperty<Owner, T>   - derived value via Owner member function ptr
 *   ComputedPropertyFn<Owner, T> - derived value via capturing lambda
 *   ConstProperty<T>             - immutable; changed exists but never fires
 *
 * All four accept a Trackable* (the owning element's 'this') for event
 * registration.  The owning class should inherit Trackable directly.
 *
 * Member declaration order: value-storage members (_value, _cache, etc.)
 * are declared BEFORE 'changed' in every class so that declaration order
 * matches constructor initialization order.  C++ initializes members in
 * declaration order; the Event constructor registers with the Trackable
 * owner, so value members must already be valid at that point.
 *
 * Connect patterns with the new Stellyra API:
 *
 *   // Method pointer - auto-tracks since T derives Trackable (preferred):
 *   prop.changed.connect( this, &MyClass::onChanged );
 *
 *   // Lambda - wrap in Callable, pass Trackable& as explicit tracker:
 *   prop.changed.connect(
 *       Callable< void( const float& ) >::create( [ this ]( const float& v ) { ... } ),
 *       *this );
 *
 * Note on const: Callable::operator() is non-const in the new Stellyra API.
 * ComputedProperty/ComputedPropertyFn therefore make get() and operator T()
 * non-const since they may invoke the callable to update the cache.
 */

#include <kmac/stellyra/callable.h>
#include <kmac/stellyra/event.h>
#include <kmac/stellyra/trackable.h>

#include <type_traits>
#include <utility>

namespace kmac {
namespace stellyra {

namespace detail {

/**
 * @brief Detects whether T supports operator== against itself.
 *
 * Property<T>/ReadOnlyProperty<Owner,T>::set() uses this to skip the
 * unchanged-value check for types with no equality comparison (e.g. plain
 * data structs), falling back to always firing changed instead of failing
 * to compile.
 */
template< typename T, typename = void >
struct IsEqualityComparable : std::false_type {};

template< typename T >
struct IsEqualityComparable< T, std::void_t< decltype( std::declval< const T& >() == std::declval< const T& >() ) > >
	: std::true_type {};

template< typename T >
inline constexpr bool IsEqualityComparable_v = IsEqualityComparable< T >::value;

} // namespace detail

/**
 * @brief Property<T>
 *
 * Declaration order: _value first, then changed - so Event construction
 * (which registers with the Trackable owner) sees an already-valid _value.
 */
template< typename T >
class Property
{
	// value storage declared first - initialized before changed
	T _value;

public:
	explicit Property( Trackable* owner, T initial = T{} );

	Property( const Property& ) = delete;
	Property& operator=( const Property& ) = delete;
	Property( Property&& ) = delete;
	Property& operator=( Property&& ) = delete;

	// -------------------------------------------------------------------------
	// read interface
	// -------------------------------------------------------------------------

	operator const T&() const;
	const T& get() const;

	// -------------------------------------------------------------------------
	// write interface
	// -------------------------------------------------------------------------

	void set( T newVal );

	/**
	 * @brief Set without firing changed - for VM pass-1 initialization.
	 *
	 * During VM instantiation, the VM sets all literal property values
	 * quietly in pass 1 before wiring any connections (pass 2), so that
	 * bindings see correct initial values without spurious emissions
	 * before anything is connected.
	 */
	void setQuiet( T newVal );

	/**
	 * @brief Set and always fire changed, even if value is unchanged.
	 *
	 * Useful when T is a container mutated in-place (e.g. a vector whose
	 * elements were modified without being replaced), where operator==
	 * would incorrectly report "no change".
	 */
	void setForce( T newVal );

	Property& operator=( T newVal );

	// -------------------------------------------------------------------------
	// reactive event - declared after _value (see file comment re: order)
	// -------------------------------------------------------------------------

	Event< const T& > changed;
};


//
// IMPLEMENTATION
//

template< typename T >
inline Property< T >::Property( Trackable* owner, T initial )
	: _value( std::move( initial ) )
	, changed( owner )
{
}

template< typename T >
inline Property< T >::operator const T&() const
{
	return _value;
}

template< typename T >
inline const T& Property< T >::get() const
{
	return _value;
}

template< typename T >
inline void Property< T >::set( T newVal )
{
	if constexpr ( detail::IsEqualityComparable_v< T > )
	{
		if ( ! ( newVal == _value ) )
		{
			_value = std::move( newVal );
			changed( _value );
		}
	}
	else
	{
		// T has no operator== - can't detect "unchanged", so always fire
		_value = std::move( newVal );
		changed( _value );
	}
}

template< typename T >
inline void Property< T >::setQuiet( T newVal )
{
	_value = std::move( newVal );
}

template< typename T >
inline void Property< T >::setForce( T newVal )
{
	_value = std::move( newVal );
	changed( _value );
}

template< typename T >
inline Property< T >& Property< T >::operator=( T newVal )
{
	set( std::move( newVal ) );
	return *this;
}


/**
 * @brief ReadOnlyProperty<Owner, T>
 *
 * Readable externally; writable only by Owner (enforced by 'friend Owner').
 * External code may freely connect to 'changed' - that is the entire point.
 */
template< typename Owner, typename T >
class ReadOnlyProperty
{
	friend Owner;

	// value storage declared first
	T _value;

public:
	explicit ReadOnlyProperty( Trackable* owner, T initial = T{} );

	ReadOnlyProperty( const ReadOnlyProperty& ) = delete;
	ReadOnlyProperty& operator=( const ReadOnlyProperty& ) = delete;
	ReadOnlyProperty( ReadOnlyProperty&& ) = delete;
	ReadOnlyProperty& operator=( ReadOnlyProperty&& ) = delete;

	// -------------------------------------------------------------------------
	// read interface - public
	// -------------------------------------------------------------------------

	operator const T&() const;
	const T& get() const;

	Event< const T& > changed;

private:
	// -------------------------------------------------------------------------
	// write interface - Owner only
	// -------------------------------------------------------------------------

	void set( T newVal );
	void setQuiet( T newVal );
	void setForce( T newVal );

	ReadOnlyProperty& operator=( T newVal );
};

template< typename Owner, typename T >
using ROProperty = ReadOnlyProperty< Owner, T >;

//
// IMPLEMENTATION
//

template< typename Owner, typename T >
inline ReadOnlyProperty< Owner, T >::ReadOnlyProperty( Trackable* owner, T initial )
	: _value( std::move( initial ) )
	, changed( owner )
{
}

template< typename Owner, typename T >
inline ReadOnlyProperty< Owner, T >::operator const T&() const
{
	return _value;
}

template< typename Owner, typename T >
inline const T& ReadOnlyProperty< Owner, T >::get() const
{
	return _value;
}

template< typename Owner, typename T >
inline void ReadOnlyProperty< Owner, T >::set( T newVal )
{
	if constexpr ( detail::IsEqualityComparable_v< T > )
	{
		if ( ! ( newVal == _value ) )
		{
			_value = std::move( newVal );
			changed( _value );
		}
	}
	else
	{
		// T has no operator== - can't detect "unchanged", so always fire
		_value = std::move( newVal );
		changed( _value );
	}
}

template< typename Owner, typename T >
inline void ReadOnlyProperty< Owner, T >::setQuiet( T newVal )
{
	_value = std::move( newVal );
}

template< typename Owner, typename T >
inline void ReadOnlyProperty< Owner, T >::setForce( T newVal )
{
	_value = std::move( newVal );
	changed( _value );
}

template< typename Owner, typename T >
inline ReadOnlyProperty< Owner, T >& ReadOnlyProperty< Owner, T >::operator=( T newVal )
{
	set( std::move( newVal ) );
	return *this;
}


/**
 * @brief ComputedProperty<Owner, T>
 *
 * Derived value computed by a const member function on Owner.
 * Zero allocation beyond the cached value - uses a plain member function
 * pointer, not std::function or Callable.
 *
 * Cache discipline:
 *   invalidate() - Owner calls this when a dependency changes.  Recomputes
 *   immediately and fires changed with the new value so downstream bindings
 *   update in the same emission cycle.  Only Owner may call invalidate().
 *
 *   get() / operator T() - non-const because a lazy recompute via _dirty
 *   mutates _cache.  Currently invalidate() always recomputes eagerly, so
 *   _dirty is always false on entry to get() in normal use; the lazy
 *   path exists as a forward-compatibility hook.
 */
template< typename Owner, typename T >
class ComputedProperty
{
	friend Owner;

	// storage declared before changed
	Owner* _owner;
	T ( Owner::*_compute )() const;
	T _cache;
	bool _dirty;

public:
	using ComputeFn = T ( Owner::* )() const;

	explicit ComputedProperty( Trackable* owner, ComputeFn fn );

	ComputedProperty( const ComputedProperty& ) = delete;
	ComputedProperty& operator=( const ComputedProperty& ) = delete;
	ComputedProperty( ComputedProperty&& ) = delete;
	ComputedProperty& operator=( ComputedProperty&& ) = delete;

	// -------------------------------------------------------------------------
	// read interface - non-const (cache update is a mutation)
	// -------------------------------------------------------------------------

	operator T();
	T get();

	Event< T > changed;

private:
	// -------------------------------------------------------------------------
	// invalidation - Owner only
	// -------------------------------------------------------------------------

	void invalidate();
};


//
// IMPLEMENTATION
//

template< typename Owner, typename T >
inline ComputedProperty< Owner, T >::ComputedProperty( Trackable* owner, ComputeFn fn )
	: _owner( static_cast< Owner* >( owner ) )
	, _compute( fn )
	, _cache( ( _owner->*fn )() )
	, _dirty( false )
	, changed( owner )
{
}

template< typename Owner, typename T >
inline ComputedProperty< Owner, T >::operator T()
{
	return get();
}

template< typename Owner, typename T >
inline T ComputedProperty< Owner, T >::get()
{
	if ( _dirty )
	{
		_cache = ( _owner->*_compute )();
		_dirty = false;
	}
	return _cache;
}

template< typename Owner, typename T >
inline void ComputedProperty< Owner, T >::invalidate()
{
	_cache = ( _owner->*_compute )();
	_dirty = false;
	changed( _cache );
}


/**
 * @brief ComputedPropertyFn<Owner, T>
 *
 * Lambda-friendly variant of ComputedProperty for cases where the compute
 * logic doesn't justify a named method, or where the compute function is
 * a runtime closure (e.g. NDL-authored bindings instantiated by the VM).
 *
 * Uses Callable<T()> internally: one heap allocation at construction via
 * Callable::create(F&&), no std::function, no virtual dispatch.
 *
 * Callable is move-only, so ComputedPropertyFn is non-copyable and
 * non-movable, consistent with all other property types.
 *
 * get() / operator T() are non-const because Callable::operator() is
 * non-const in the new Stellyra API.
 */
template< typename Owner, typename T >
class ComputedPropertyFn
{
	friend Owner;

	// storage declared before changed
	Callable< T() > _compute;
	T _cache;
	bool _dirty;

public:
	template< typename F >
	explicit ComputedPropertyFn( Trackable* owner, F&& fn );

	ComputedPropertyFn( const ComputedPropertyFn& ) = delete;
	ComputedPropertyFn& operator=( const ComputedPropertyFn& ) = delete;
	ComputedPropertyFn( ComputedPropertyFn&& ) = delete;
	ComputedPropertyFn& operator=( ComputedPropertyFn&& ) = delete;

	// -------------------------------------------------------------------------
	// read interface - non-const (Callable::operator() is non-const)
	// -------------------------------------------------------------------------

	operator T();
	T get();

	Event< T > changed;

private:
	// -------------------------------------------------------------------------
	// invalidation - Owner only
	// -------------------------------------------------------------------------

	void invalidate();
};


//
// IMPLEMENTATION
//

template< typename Owner, typename T >
template< typename F >
inline ComputedPropertyFn< Owner, T >::ComputedPropertyFn( Trackable* owner, F&& fn )
	: _compute( Callable< T() >::create( std::forward< F >( fn ) ) )
	, _cache( _compute() )
	, _dirty( false )
	, changed( owner )
{
	static_assert(
		std::is_invocable_r_v< T, F >,
		"ComputedPropertyFn: fn must be callable as T()" );
}

template< typename Owner, typename T >
inline ComputedPropertyFn< Owner, T >::operator T()
{
	return get();
}

template< typename Owner, typename T >
inline T ComputedPropertyFn< Owner, T >::get()
{
	if ( _dirty )
	{
		_cache = _compute();
		_dirty = false;
	}
	return _cache;
}

template< typename Owner, typename T >
inline void ComputedPropertyFn< Owner, T >::invalidate()
{
	_cache = _compute();
	_dirty = false;
	changed( _cache );
}


/**
 * @brief ConstProperty<T>
 *
 * Immutable after construction.  'changed' exists for API uniformity so that
 * binding code can connect to any property type without special-casing, but
 * it never fires.  The semantic analyzer uses this to perform dead binding
 * elimination: bindings whose entire dependency set is ConstProperty values
 * compile to SET_PROPERTY opcodes rather than live BIND_PROPERTY connections.
 */
template< typename T >
class ConstProperty
{
	// value declared before changed
	const T _value;

public:
	explicit ConstProperty( Trackable* owner, T value );

	ConstProperty( const ConstProperty& ) = delete;
	ConstProperty& operator=( const ConstProperty& ) = delete;
	ConstProperty( ConstProperty&& ) = delete;
	ConstProperty& operator=( ConstProperty&& ) = delete;

	operator const T&() const;
	const T& get() const;

	Event< const T& > changed;
};


//
// IMPLEMENTATION
//

template< typename T >
inline ConstProperty< T >::ConstProperty( Trackable* owner, T value )
	: _value( std::move( value ) )
	, changed( owner )
{
}

template< typename T >
inline ConstProperty< T >::operator const T&() const
{
	return _value;
}

template< typename T >
inline const T& ConstProperty< T >::get() const
{
	return _value;
}

} // namespace stellyra
} // namespace kmac

#endif // KMAC_STELLYRA_PROPERTY_H
