#ifndef FIONA_DETAIL_CONFIG_HPP
#define FIONA_DETAIL_CONFIG_HPP

#include <boost/config.hpp>

#if defined( FIONA_DYN_LINK )
#if defined( FIONA_SOURCE )
#define FIONA_DECL BOOST_SYMBOL_EXPORT
#else
#define FIONA_DECL BOOST_SYMBOL_IMPORT
#endif

#if defined( FIONA_TLS_SOURCE )
#define FIONA_TLS_DECL BOOST_SYMBOL_EXPORT
#else
#define FIONA_TLS_DECL BOOST_SYMBOL_IMPORT
#endif

#else
#define FIONA_DECL
#define FIONA_TLS_DECL
#endif

#endif // FIONA_DETAIL_CONFIG_HPP
