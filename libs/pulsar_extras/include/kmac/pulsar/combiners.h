#ifndef KMAC_PULSAR_COMBINERS_H
#define KMAC_PULSAR_COMBINERS_H

/**
 * @file combiners.h
 * @brief Standard combiner functors for use with CombiningEvent.
 *
 * A combiner is a callable that accepts a pair of iterators over a range of
 * return values collected from all connected handlers, and produces a single
 * combined result.
 *
 * All combiners follow the same interface:
 * @code
 * ReturnType operator()(Iter first, Iter last) const;
 * @endcode
 *
 * Usage with CombiningEvent:
 * @code
 * CombiningEvent<bool, Combiners::LogicalAnd<>, std::string> validate{this};
 * validate.connect(checker1, &Checker::check);
 * validate.connect(checker2, &Checker::check);
 * bool allOk = validate("input");   // true only if every handler returns true
 * @endcode
 *
 * All combiners return a default-constructed value when the input range is
 * empty (i.e. no handlers are connected).
 */

#include <algorithm>
#include <functional>
#include <numeric>

namespace kmac {
namespace pulsar {
namespace Combiners {

/**
 * @brief Returns true if all handlers returned true (logical AND).
 *
 * Returns true for an empty range.
 */
template< typename T = bool >
struct LogicalAnd
{
	using result_type = bool;

	template< typename Iter >
	bool operator()( Iter first, Iter last ) const
	{
		return std::all_of( first, last, []( bool v ) { return v; } );
	}
};

/**
 * @brief Returns true if at least one handler returned true (logical OR).
 *
 * Returns false for an empty range.
 */
template< typename T = bool >
struct LogicalOr
{
	using result_type = bool;

	template< typename Iter >
	bool operator()( Iter first, Iter last ) const
	{
		return std::any_of( first, last, []( bool v ) { return v; } );
	}
};

/**
 * @brief Returns the sum of all handler return values.
 *
 * Returns T{} for an empty range.
 */
template< typename T >
struct Sum
{
	using result_type = T;

	template< typename Iter >
	T operator()( Iter first, Iter last ) const
	{
		return std::accumulate( first, last, T{} );
	}
};

/**
 * @brief Returns the product of all handler return values.
 *
 * Returns T{1} for an empty range.
 */
template< typename T >
struct Product
{
	using result_type = T;

	template< typename Iter >
	T operator()( Iter first, Iter last ) const
	{
		return std::accumulate( first, last, T{ 1 }, std::multiplies< T >() );
	}
};

/**
 * @brief Returns the arithmetic mean of all handler return values.
 *
 * Returns T{} for an empty range.
 */
template< typename T >
struct Mean
{
	using result_type = T;

	template< typename Iter >
	T operator()( Iter first, Iter last ) const
	{
		if ( first == last )
		{
			return T{};
		}

		auto sum = std::accumulate( first, last, T{} );
		auto count = std::distance( first, last );
		return sum / static_cast< T >( count );
	}
};

// alias when 'Average' preferred over 'Mean'
template< typename T >
using Average = Mean< T >;

/**
 * @brief Returns the maximum handler return value.
 *
 * Returns T{} for an empty range.
 */
template< typename T >
struct Maximum
{
	using result_type = T;

	template< typename Iter >
	T operator()( Iter first, Iter last ) const
	{
		return first == last ? T{} : *std::max_element( first, last );
	}
};

/**
 * @brief Returns the minimum handler return value.
 *
 * Returns T{} for an empty range.
 */
template< typename T >
struct Minimum
{
	using result_type = T;

	template< typename Iter >
	T operator()( Iter first, Iter last ) const
	{
		return first == last ? T{} : *std::min_element( first, last );
	}
};

/**
 * @brief Counts how many handlers returned true.
 *
 * Returns 0 for an empty range.
 */
struct CountTrue
{
	using result_type = int;

	template< typename Iter >
	int operator()( Iter first, Iter last ) const
	{
		return std::count( first, last, true );
	}
};

/**
 * @brief Returns only the first handler's return value.
 *
 * Useful when only one handler's result should matter and the rest exist
 * purely as observers.  "First" means connection order (CombiningEvent has
 * no priority mechanism) - the order handlers were connected in, regardless
 * of which underlying storage slot each one currently occupies.  Returns
 * T{} for an empty range.
 */
template< typename T >
struct First
{
	using result_type = T;

	template< typename Iter >
	T operator()( Iter first, Iter last ) const
	{
		return first == last ? T{} : *first;
	}
};

/**
 * @brief Returns the first non-default return value encountered.
 *
 * Iterates through results in connection order (see First) and returns the
 * first value that compares unequal to T{}.  Returns T{} if all values are
 * default or the range is empty.
 */
template< typename T >
struct FirstNonDefault
{
	using result_type = T;

	template< typename Iter >
	T operator()( Iter first, Iter last ) const
	{
		auto it = std::find_if( first, last, []( const T& v ) { return v != T{}; } );
		return it != last ? *it : T{};
	}
};

/**
 * @brief Returns only the last handler's return value.
 *
 * Useful when only the final handler's result matters - "final" in
 * connection order (see First), the order handlers were connected in.
 * Returns T{} for an empty range.
 */
template< typename T >
struct Last
{
	using result_type = T;

	template< typename Iter >
	T operator()( Iter first, Iter last ) const
	{
		if ( first == last )
		{
			return T{};
		}

		return *std::prev( last );
	}
};

/**
 * @brief Returns the last non-default return value encountered.
 *
 * Iterates through results in reverse connection order (see First) and
 * returns the last value that compares unequal to T{}.  Returns T{} if all
 * values are default or the range is empty.
 */
template< typename T >
struct LastNonDefault
{
	using result_type = T;

	template< typename Iter >
	T operator()( Iter first, Iter last ) const
	{
		auto it = std::find_if(
			std::make_reverse_iterator( last ),
			std::make_reverse_iterator( first ),
			[]( const T& v ) { return v != T{}; } );
		return it != std::make_reverse_iterator( first ) ? *it : T{};
	}
};

} // namespace Combiners
} // namespace pulsar
} // namespace kmac

#endif // KMAC_PULSAR_COMBINERS_H
