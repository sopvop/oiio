/*
  Copyright 2008 Larry Gritz and the other authors and contributors.
  All Rights Reserved.

  Redistribution and use in source and binary forms, with or without
  modification, are permitted provided that the following conditions are
  met:
  * Redistributions of source code must retain the above copyright
    notice, this list of conditions and the following disclaimer.
  * Redistributions in binary form must reproduce the above copyright
    notice, this list of conditions and the following disclaimer in the
    documentation and/or other materials provided with the distribution.
  * Neither the name of the software's owners nor the names of its
    contributors may be used to endorse or promote products derived from
    this software without specific prior written permission.
  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
  A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
  OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
  SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
  LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
  DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
  THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
  (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
  OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

  (This is the Modified BSD License)
*/

#include <cstdio>
#include <string>
#include <vector>

#include "export.h"
#include "thread.h"
#include "strutil.h"
#include "hash.h"
#include "dassert.h"

#include "ustring.h"


#if 0
// Use reader/writer locks
typedef shared_mutex ustring_mutex_t;
typedef shared_lock ustring_read_lock_t;
typedef unique_lock ustring_write_lock_t;
#elif 0
// Use regular mutex
typedef mutex ustring_mutex_t;
typedef lock_guard ustring_read_lock_t;
typedef lock_guard ustring_write_lock_t;
#elif 1
// Use spin locks
typedef spin_mutex ustring_mutex_t;
typedef spin_lock ustring_read_lock_t;
typedef spin_lock ustring_write_lock_t;
#else
// Use null locks
typedef null_mutex ustring_mutex_t;
typedef null_lock<null_mutex> ustring_read_lock_t;
typedef null_lock<null_mutex> ustring_write_lock_t;
#endif


#ifdef OIIO_HAVE_BOOST_UNORDERED_MAP
typedef boost::unordered_map <const char *, ustring::TableRep *, Strutil::StringHash, Strutil::StringEqual> UstringTable;
#else
typedef hash_map <const char *, ustring::TableRep *, Strutil::StringHash, Strutil::StringEqual> UstringTable;
#endif // OIIO_HAVE_BOOST_UNORDERED_MAP

std::string ustring::empty_std_string ("");


namespace { // anonymous

static long long ustring_stats_memory = 0;
static long long ustring_stats_constructed = 0;
static long long ustring_stats_unique = 0;
static ustring_mutex_t ustring_mutex;

};          // end anonymous namespace



ustring::TableRep::TableRep (const char *s, size_t len)
    : hashed(Strutil::strhash(s))
{
    strcpy ((char *)c_str(), s);
    length = len;
    dummy_capacity = len;
    dummy_refcount = 1;   // so it never frees

#if defined(__GNUC__)
    // We don't want the internal 'string str' to redundantly store the
    // chars, along with our own allocation.  So we use our knowledge of
    // the internal structure of gcc strings to make it point to our chars!
    // Note that we've carefully structured the TableRep fields so they
    // mimic a GCC basic_string::_Rep.
    //
    // It turns out that the first field of a gcc std::string is a
    // pointer to the characters within the basic_string::_Rep.  We
    // merely redirect that pointer, though for std::string to function
    // properly, the chars must be preceeded immediately in memory by
    // the rest of basic_string::_Rep, consisting of length, capacity
    // and refcount fields.  And we have designed our TableRep to do
    // just that!  So now we redirect the std::string's pointer to our
    // own characters and its mocked-up _Rep.  
    //
    // See /usr/include/c++/VERSION/bits/basic_string.h for the details
    // of gcc's std::string implementation.

    *(const char **)&str = c_str();
    DASSERT (str.c_str() == c_str());
#else
    // Not gcc -- just assign the internal string.  This will result in
    // double allocation for the chars.  If you care about that, do
    // something special for your platform, much like we did for gcc
    // above.  (Windows users, I'm talking to you.)
    str = s;
#endif
}



ustring::TableRep::~TableRep ()
{
#if defined(__GNUC__)
    // Doctor the string to be empty again before destroying.
    ASSERT (str.c_str() == c_str());
    std::string empty;
    memcpy (&str, &empty, sizeof(std::string));
#endif
}



const ustring::TableRep *
ustring::_make_unique (const char *str)
{
    static UstringTable ustring_table;

    // Eliminate NULLs
    if (! str)
        str = "";

    // Check the ustring table to see if this string already exists.  If so,
    // construct from its canonical representation.
    {
        // Grab a read lock on the table.  Hopefully, the string will
        // already be present, and we can immediately return its rep.
        // Lots of threads may do this simultaneously, as long as they
        // are all in the table.
        ustring_read_lock_t read_lock (ustring_mutex);
        ++ustring_stats_constructed;
        UstringTable::const_iterator found = ustring_table.find (str);
        if (found != ustring_table.end())
           return found->second;
    }

    // This string is not yet in the ustring table.  Create a new entry.
    // Note that we are speculatively releasing the lock and building the
    // string locally.  Then we'll lock again to put it in the table.
    size_t len = strlen(str);
    size_t size = sizeof(ustring::TableRep) + len + 1;
    ustring::TableRep *rep = (ustring::TableRep *) malloc (size);
    new (rep) ustring::TableRep (str, len);

    UstringTable::const_iterator found;
    {
        // Now grab a write lock on the table.  This will prevent other
        // threads from even reading.  Just in case another thread has
        // already entered this thread while we were unlocked and
        // constructing its rep, check the table one more time.  If it's
        // still empty, add it.
        ustring_write_lock_t write_lock (ustring_mutex);
        found = ustring_table.find (str);
        if (found == ustring_table.end()) {
            ustring_table[rep->c_str()] = rep;
            ++ustring_stats_unique;
            ustring_stats_memory += size;
#ifndef __GNUC__
            ustring_stats_memory += len+1;  // non-GNU replicates the chars
#endif
            return rep;
        }
    }
    // Somebody else added this string to the table in that interval
    // when we were unlocked and constructing the rep.  Don't use the
    // new one!  Use the one in the table and disregard the one we
    // speculatively built.  Note that we've already released the lock
    // on the table at this point.
    delete rep;
    return found->second;
}



ustring
ustring::format (const char *fmt, ...)
{
    // Allocate a buffer on the stack that's big enough for us almost
    // all the time.  Be prepared to allocate dynamically if it doesn't fit.
    size_t size = 1024;
    char stackbuf[1024];
    std::vector<char> dynamicbuf;
    char *buf = &stackbuf[0];
    
    while (1) {
        // Try to vsnprintf into our buffer.
        va_list ap;
        va_start (ap, fmt);
        int needed = vsnprintf (buf, size, fmt, ap);
        va_end (ap);

        // NB. C99 (which modern Linux and OS X follow) says vsnprintf
        // failure returns the length it would have needed.  But older
        // glibc and current Windows return -1 for failure, i.e., not
        // telling us how much was needed.

        if (needed < (int)size && needed >= 0) {
            // It fit fine so we're done.
            return ustring (buf);
        }

        // vsnprintf reported that it wanted to write more characters
        // than we allotted.  So try again using a dynamic buffer.  This
        // doesn't happen very often if we chose our initial size well.
        size = (needed > 0) ? (needed+1) : (size*2);
        dynamicbuf.resize (size);
        buf = &dynamicbuf[0];
    }
}



std::string
ustring::getstats (bool verbose)
{
    ustring_read_lock_t read_lock (ustring_mutex);
    std::ostringstream out;
    if (verbose) {
        out << "ustring statistics:\n";
        out << "  ustring requests: " << ustring_stats_constructed
            << ", unique " << ustring_stats_unique << "\n";
        out << "  ustring memory: " << Strutil::memformat(ustring_stats_memory)
            << "\n";
    } else {
        out << "requests: " << ustring_stats_constructed
            << ", unique " << ustring_stats_unique
            << ", " << Strutil::memformat(ustring_stats_memory);
    }
    return out.str();
}




size_t
ustring::memory ()
{
    ustring_read_lock_t read_lock (ustring_mutex);
    return ustring_stats_memory;
}
