#ifndef KMAC_PULSAR_PROPERTY_H
#define KMAC_PULSAR_PROPERTY_H

/**
 * @file property.h
 * @brief Observable property types built on top of Pulsar events.
 *
 * Four property flavours cover the most common data-binding patterns:
 *
 * | Type                            | Read   | Write                  | Value source     |
 * |---------------------------------|--------|------------------------|------------------|
 * | Property<T>                     | anyone | anyone                 | stored           |
 * | ReadOnlyProperty<Owner, T>      | anyone | owner only             | stored           |
 * | ComputedProperty<Owner, Fn, T>  | anyone | owner via invalidate() | cached fn        |
 * | ConstProperty<T>                | anyone | nobody                 | set at construct |
 *
 * All four expose a public @c changed event.  Connection syntax is identical
 * to any other Pulsar event:
 *
 * @code
 * obj->temperature.changed.connect( receiver, &Receiver::onTemperature );
 * @endcode
 *
 * @section event_sig Event signature
 *
 * @c changed carries @c const @c T& in all cases.  For Direct connections the
 * reference is into the property's stored value, valid for the duration of the
 * synchronous dispatch.  For Deferred connections Pulsar stores arguments in a
 * @c shared_ptr<tuple<decay_t<Args>...>>, copying the value at emission time,
 * so the stored copy remains valid when the handler eventually runs regardless
 * of what happens to the property in the interim.
 *
 * @section equality Equality guard
 *
 * @c Property and @c ReadOnlyProperty skip emission when the new value equals
 * the stored value (detected via @c operator== when available).  Use @c setForce()
 * to emit unconditionally, and @c setQuiet() to update the stored value without
 * emitting at all.
 *
 * @section computed ComputedProperty
 *
 * @c ComputedProperty holds a cached value.  The owner calls @c invalidate()
 * whenever a dependency changes; this recomputes the value and fires @c changed.
 * Between invalidations @c get() returns the cached result without re-evaluating
 * the compute function.  Dependency subscription is the caller's responsibility.
 *
 * @section const_prop ConstProperty
 *
 * @c ConstProperty stores a value set at construction and never changes it.
 * @c changed exists but never fires, allowing uniform connection code that does
 * not need to special-case constant properties.
 */

#include <kmac/pulsar.h>

#include <type_traits>
#include <utility>

namespace kmac {
namespace pulsar {

// ============================================================================
// Internal helper: detect operator== without requiring it
// ============================================================================

namespace detail {

template< typename T, typename = void >
struct HasEqualityOp : std::false_type {};

template< typename T >
struct HasEqualityOp< T, std::void_t< decltype( std::declval< T >() == std::declval< T >() ) > >
	: std::true_type {};

template< typename T >
inline constexpr bool hasEqualityOp = HasEqualityOp< T >::value;

} // namespace detail


// ============================================================================
// Property<T>
// Readable and writable by anyone.
// ============================================================================

/**
 * @brief Observable value readable and writable by any code.
 *
 * @tparam T value type; should be copy-constructible and copy-assignable.
 *   If @p T provides @c operator==, assignments that do not change the value
 *   are silently ignored.  Use @c setForce() to bypass this guard.
 *
 * @code
 * class Config : public pulsar::Object
 * {
 * public:
 *     pulsar::Property< int > maxRetries { this, 3 };
 * };
 *
 * config->maxRetries = 5;            // fires changed(5)
 * config->maxRetries = 5;            // no-op - value unchanged
 * config->maxRetries.setForce( 5 );  // fires changed(5) regardless
 * @endcode
 */
template< typename T >
class Property
{
private:
	T _value;

public:
	/**
	 * @brief Construct with owning sender and initial value.
	 *
	 * @param sender Object that logically owns this property; typically
	 *   @c this of the enclosing class
	 * @param initial starting value
	 */
	explicit Property( Object* sender, T initial = T{} );

	Property( const Property& ) = delete;
	Property& operator=( const Property& ) = delete;
	Property( Property&& ) = delete;
	Property& operator=( Property&& ) = delete;

	/** @brief Fires with the new value whenever the stored value changes. */
	Event< const T& > changed;

	// -------------------------------------------------------------------------
	// read
	// -------------------------------------------------------------------------

	/** @brief Return the current value. */
	const T& get() const;

	/** @brief Implicit conversion - allows @c T v = obj->property; */
	operator const T&() const;

	// -------------------------------------------------------------------------
	// write
	// -------------------------------------------------------------------------

	/**
	 * @brief Set the value; fires @c changed if the value is different.
	 *
	 * Equality is tested via @c operator== when available.  For types without
	 * @c operator==, every call fires @c changed - use @c setForce() explicitly
	 * to make that intent clear.
	 */
	void set( T newValue );

	/** @brief Convenience assignment; equivalent to @c set(). */
	Property& operator=( T newValue );

	/**
	 * @brief Set the value and fire @c changed unconditionally.
	 *
	 * Bypasses the equality guard.  Useful when @p T has no @c operator==,
	 * or when downstream observers must always be notified.
	 */
	void setForce( T newValue );

	/**
	 * @brief Update the stored value without firing @c changed.
	 *
	 * Useful for silent initialisation or resetting state without side effects.
	 */
	void setQuiet( T newValue );
};


// ============================================================================
// ReadOnlyProperty<Owner, T>
// Readable by anyone; writable only by Owner.
// ============================================================================

/**
 * @brief Observable value readable by anyone but writable only by @p Owner.
 *
 * @tparam Owner the class permitted to call @c set(), @c setForce(),
 *   @c setQuiet(), and @c operator=()
 * @tparam T value type
 *
 * @code
 * class Motor : public pulsar::Object
 * {
 * public:
 *     pulsar::ReadOnlyProperty< Motor, int > rpm { this, 0 };
 *
 *     void setRpm( int v ) { rpm = v; }   // OK - Motor is Owner
 * };
 *
 * motor->rpm.get();                   // OK - anyone can read
 * motor->rpm.changed.connect( ... );  // OK - anyone can observe
 * motor->rpm = 1000;                  // compile error
 * @endcode
 */
template< typename Owner, typename T >
class ReadOnlyProperty
{
	friend Owner;

private:
	T _value;

public:
	/**
	 * @brief Construct with owning sender and initial value.
	 *
	 * @param sender Object that logically owns this property
	 * @param initial starting value
	 */
	explicit ReadOnlyProperty( Object* sender, T initial = T{} );

	ReadOnlyProperty( const ReadOnlyProperty& ) = delete;
	ReadOnlyProperty& operator=( const ReadOnlyProperty& ) = delete;
	ReadOnlyProperty( ReadOnlyProperty&& ) = delete;
	ReadOnlyProperty& operator=( ReadOnlyProperty&& ) = delete;

	/** @brief Fires with the new value whenever the stored value changes. */
	Event< const T& > changed;

	// -------------------------------------------------------------------------
	// read - always public
	// -------------------------------------------------------------------------

	/** @brief Return the current value. */
	const T& get() const;

	/** @brief Implicit conversion - allows @c T v = obj->property; */
	operator const T&() const;

private:
	// -------------------------------------------------------------------------
	// write - Owner only
	// -------------------------------------------------------------------------

	/** @copydoc Property::set */
	void set( T newValue );

	/** @copydoc Property::operator= */
	ReadOnlyProperty& operator=( T newValue );

	/** @copydoc Property::setForce */
	void setForce( T newValue );

	/** @copydoc Property::setQuiet */
	void setQuiet( T newValue );
};


// ============================================================================
// ComputedProperty<Owner, ComputeFn, T>
// Read-only cached value derived from a compute function.
// Owner calls invalidate() when dependencies change.
// ============================================================================

/**
 * @brief Observable cached value derived from a compute function.
 *
 * The compute function is evaluated once at construction to seed the cache,
 * then again each time @p Owner calls @c invalidate().  Between invalidations
 * @c get() returns the cached value without re-evaluating the function.
 *
 * @tparam Owner the class permitted to call @c invalidate()
 * @tparam ComputeFn callable type: must be invocable with no arguments and
 *   return a value convertible to @p T.  A capturing lambda is the most
 *   natural choice; the type is deduced via CTAD or the @c makeComputed()
 *   factory helper.
 * @tparam T the cached value type; deduced from @p ComputeFn's return type
 *   if not specified explicitly
 *
 * In C++17 the @p ComputeFn type must be nameable at the declaration site.
 * A callable struct is the cleanest approach:
 *
 * @code
 * class Rectangle : public pulsar::Object
 * {
 * public:
 *     pulsar::Property< int > width  { this, 0 };
 *     pulsar::Property< int > height { this, 0 };
 *
 *     struct AreaFn
 *     {
 *         Rectangle* self;
 *         int operator()() const { return self->width.get() * self->height.get(); }
 *     };
 *
 *     pulsar::ComputedProperty< Rectangle, AreaFn > area { this, AreaFn{ this } };
 * };
 * @endcode
 *
 * In C++20, a lambda type can be named directly via @c decltype, removing
 * the need for the callable struct.  Dependency wiring via @c connect follows
 * the same pattern regardless - see the @c makeComputed() factory for an
 * alternative that deduces the function type automatically.
 *
 * @note Dependency wiring is the caller's responsibility.  @c ComputedProperty
 *   does not inspect or subscribe to dependencies automatically.
 *
 * @see makeComputed() for a factory that deduces @p ComputeFn and @p T.
 */
template< typename Owner, typename ComputeFn, typename T = std::invoke_result_t< ComputeFn > >
class ComputedProperty
{
	friend Owner;

private:
	ComputeFn _compute;
	T _cache;

public:
	/**
	 * @brief Construct and evaluate the compute function to seed the cache.
	 *
	 * @param sender Object that logically owns this property
	 * @param fn compute function; captured by value - ensure any references
	 *   it captures remain valid for the lifetime of the property
	 */
	explicit ComputedProperty( Object* sender, ComputeFn fn );

	ComputedProperty( const ComputedProperty& ) = delete;
	ComputedProperty& operator=( const ComputedProperty& ) = delete;
	ComputedProperty( ComputedProperty&& ) = delete;
	ComputedProperty& operator=( ComputedProperty&& ) = delete;

	/** @brief Fires with the newly computed value each time @c invalidate() is called. */
	Event< const T& > changed;

	// -------------------------------------------------------------------------
	// read - always public
	// -------------------------------------------------------------------------

	/** @brief Return the cached value. */
	const T& get() const;

	/** @brief Implicit conversion - allows @c T v = obj->property; */
	operator const T&() const;

private:
	// -------------------------------------------------------------------------
	// invalidation - owner only
	// -------------------------------------------------------------------------

	/**
	 * @brief Recompute the cached value and fire @c changed.
	 *
	 * Call this from @p Owner whenever any dependency of the compute function
	 * changes.  Fires @c changed unconditionally - the compute function is
	 * assumed to produce a meaningfully different result when called.
	 */
	void invalidate();
};


// ============================================================================
// ConstProperty<T>
// Set once at construction; changed exists but never fires.
// ============================================================================

/**
 * @brief Observable value that is fixed at construction and never changes.
 *
 * @c changed is present and connectable but never fires, allowing uniform
 * binding code that does not need to special-case constant properties.
 *
 * @tparam T value type
 *
 * @code
 * class Element : public pulsar::Object
 * {
 * public:
 *     pulsar::ConstProperty< std::string > typeName { this, "Button" };
 * };
 * @endcode
 */
template< typename T >
class ConstProperty
{
private:
	const T _value;

public:
	/**
	 * @brief Construct with the fixed value.
	 *
	 * @param sender Object that logically owns this property
	 * @param value the permanent value
	 */
	explicit ConstProperty( Object* sender, T value );

	ConstProperty( const ConstProperty& ) = delete;
	ConstProperty& operator=( const ConstProperty& ) = delete;
	ConstProperty( ConstProperty&& ) = delete;
	ConstProperty& operator=( ConstProperty&& ) = delete;

	/** @brief Present for interface uniformity; never fires. */
	Event< const T& > changed;

	// -------------------------------------------------------------------------
	// read
	// -------------------------------------------------------------------------

	/** @brief Return the fixed value. */
	const T& get() const;

	/** @brief Implicit conversion - allows @c T v = obj->property; */
	operator const T&() const;
};


//
// Property
//
template< typename T >
Property< T >::Property( Object* sender, T initial )
	: _value( std::move( initial ) )
	, changed( sender )
{
}

template< typename T >
const T& Property< T >::get() const
{
	return _value;
}

template< typename T >
Property< T >::operator const T&() const
{
	return _value;
}

template< typename T >
void Property< T >::set( T newValue )
{
	if constexpr( detail::hasEqualityOp< T > )
	{
		if ( _value == newValue )
		{
			return;
		}
	}
	_value = std::move( newValue );
	changed( _value );
}

template< typename T >
Property< T >& Property< T >::operator=( T newValue )
{
	set( std::move( newValue ) );
	return *this;
}

template< typename T >
void Property< T >::setForce( T newValue )
{
	_value = std::move( newValue );
	changed( _value );
}

template< typename T >
void Property< T >::setQuiet( T newValue )
{
	_value = std::move( newValue );
}


//
// ReadOnlyProperty
//

template< typename Owner, typename T >
ReadOnlyProperty< Owner, T >::ReadOnlyProperty( Object* sender, T initial )
	: _value( std::move( initial ) )
	, changed( sender )
{
}

template< typename Owner, typename T >
const T& ReadOnlyProperty< Owner, T >::get() const
{
	return _value;
}

template< typename Owner, typename T >
ReadOnlyProperty< Owner, T >::operator const T&() const
{
	return _value;
}

template< typename Owner, typename T >
void ReadOnlyProperty< Owner, T >::set( T newValue )
{
	if constexpr( detail::hasEqualityOp< T > )
	{
		if ( _value == newValue )
		{
			return;
		}
	}
	_value = std::move( newValue );
	changed( _value );
}

template< typename Owner, typename T >
ReadOnlyProperty< Owner, T >& ReadOnlyProperty< Owner, T >::operator=( T newValue )
{
	set( std::move( newValue ) );
	return *this;
}

template< typename Owner, typename T >
void ReadOnlyProperty< Owner, T >::setForce( T newValue )
{
	_value = std::move( newValue );
	changed( _value );
}

template< typename Owner, typename T >
void ReadOnlyProperty< Owner, T >::setQuiet( T newValue )
{
	_value = std::move( newValue );
}


//
// ComputedProperty
//

template< typename Owner, typename ComputeFn, typename T >
ComputedProperty< Owner, ComputeFn, T >::ComputedProperty( Object* sender, ComputeFn fn )
	: _compute( std::move( fn ) )
	, _cache( _compute() )
	, changed( sender )
{
}

template< typename Owner, typename ComputeFn, typename T >
const T& ComputedProperty< Owner, ComputeFn, T >::get() const
{
	return _cache;
}

template< typename Owner, typename ComputeFn, typename T >
ComputedProperty< Owner, ComputeFn, T >::operator const T&() const
{
	return _cache;
}

template< typename Owner, typename ComputeFn, typename T >
void ComputedProperty< Owner, ComputeFn, T >::invalidate()
{
	_cache = _compute();
	changed( _cache );
}


//
// ConstProperty
//

template< typename T >
ConstProperty< T >::ConstProperty( Object* sender, T value )
	: _value( std::move( value ) )
	, changed( sender )
{
}

template< typename T >
const T& ConstProperty< T >::get() const
{
	return _value;
}

template< typename T >
ConstProperty< T >::operator const T&() const
{
	return _value;
}


// ============================================================================
// Factory helper
// ============================================================================

/**
 * @brief Construct a ComputedProperty, deducing ComputeFn and T automatically.
 *
 * Because @p Owner cannot be deduced from the constructor arguments it must
 * be supplied explicitly; @p ComputeFn and @p T are deduced from @p fn:
 *
 * @code
 * auto area = pulsar::makeComputed< Rectangle >( this, [this] {
 *     return width.get() * height.get();
 * } );
 * @endcode
 *
 * The returned object is typically stored as a member.  The above form is
 * most useful in factory functions or tests; for member declarations the
 * explicit template form is still required.
 *
 * @tparam Owner class permitted to call @c invalidate()
 * @tparam ComputeFn callable type (deduced)
 */
template< typename Owner, typename ComputeFn >
auto makeComputed( Object* sender, ComputeFn&& fn )
	-> ComputedProperty< Owner, std::decay_t< ComputeFn > >
{
	return ComputedProperty< Owner, std::decay_t< ComputeFn > >(
		sender, std::forward< ComputeFn >( fn ) );
}


// ============================================================================
// Aliases
// ============================================================================

/** @brief Shorter alias for ReadOnlyProperty. */
template< typename Owner, typename T >
using ROProperty = ReadOnlyProperty< Owner, T >;

} // namespace pulsar
} // namespace kmac

#endif // KMAC_PULSAR_PROPERTY_H
