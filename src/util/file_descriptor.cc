/* -*-mode:c++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */

#include "file_descriptor.hh"

#include <unistd.h>
#include <fcntl.h>
#include <cassert>
#include <sys/file.h>

#include "exception.hh"
#include "epoller.hh"

using namespace std;

/* construct from fd number */
FileDescriptor::FileDescriptor( const int fd )
  : fd_( fd ),
   eof_( false ),
   read_count_( 0 ),
   write_count_( 0 ),
   epollers_()
{
  /* set close-on-exec flag so our file descriptors
    aren't passed on to unrelated children (like a shell) */
  CheckSystemCall( "fcntl FD_CLOEXEC", fcntl( fd_, F_SETFD, FD_CLOEXEC ) );
}

/* move constructor */
FileDescriptor::FileDescriptor( FileDescriptor && other )
  : fd_( other.fd_ ),
   eof_( other.eof_ ),
   read_count_( other.read_count_ ),
   write_count_( other.write_count_ ),
   epollers_( move( other.epollers_ ) )
{
  /* mark other file descriptor as inactive */
  other.fd_ = -1;
}

/* close method throws exception on failure */
void FileDescriptor::close()
{
  if ( fd_ < 0 ) { /* has already been moved away or closed */
    return;
  }

  /* notify attached epollers */
  for (const auto & epoller_item : epollers_) {
    auto epoller_shared_ptr = epoller_item.second.lock();
    if (epoller_shared_ptr) {
      epoller_shared_ptr->deregister(*this);
    }
  }

  CheckSystemCall( "close", ::close( fd_ ) );

  fd_ = -1;
}

/* destructor tries to close, but catches exception */
FileDescriptor::~FileDescriptor()
{
  try {
    close();
  } catch ( const exception & e ) { /* don't throw from destructor */
    print_exception( "FileDescriptor", e );
  }
}

/* attempt to write a portion of a string */
string::const_iterator FileDescriptor::write( const string::const_iterator & begin,
                       const string::const_iterator & end )
{
  if ( begin >= end ) {
    throw runtime_error( "nothing to write" );
  }

  ssize_t bytes_written = CheckSystemCall( "write", ::write( fd_, &*begin, end - begin ) );
  if ( bytes_written == 0 ) {
    throw runtime_error( "write returned 0" );
  }

  register_write();

  return begin + bytes_written;
}

/* read method */
string FileDescriptor::read( const size_t limit )
{
  char buffer[ BUFFER_SIZE ];

  ssize_t bytes_read = CheckSystemCall( "read", ::read( fd_, buffer, min( BUFFER_SIZE, limit ) ) );
  if ( bytes_read == 0 ) {
    set_eof();
  }

  register_read();

  return string( buffer, bytes_read );
}

/* write method */
string::const_iterator FileDescriptor::write( const std::string & buffer, const bool write_all )
{
  auto it = buffer.begin();

  do {
    it = write( it, buffer.end() );
  } while ( write_all and (it != buffer.end()) );

  return it;
}

string FileDescriptor::read_exactly( const size_t length,
                                     const bool fail_silently )
  {
    std::string ret;

    while ( ret.size() < length ) {
      ret.append( read( length - ret.size() ) );
      if ( eof() ) {
        if ( fail_silently ) {
          return ret;
        }
        else {
          throw std::runtime_error( "read_exactly: reached EOF before reaching target" );
        }
      }
    }

    assert( ret.size() == length );
    return ret;
  }

void FileDescriptor::block_for_exclusive_lock()
{
  CheckSystemCall( "flock", flock( fd_num(), LOCK_EX ) );
}

void FileDescriptor::release_flock()
{
  CheckSystemCall( "flock", flock( fd_num(), LOCK_UN ) );
}

void FileDescriptor::set_blocking( const bool block )
{
  int flags = CheckSystemCall( "fcntl F_GETFL", fcntl( fd_, F_GETFL ) );

  if ( block ) {
    flags = flags & ~O_NONBLOCK;
  } else {
    flags = flags | O_NONBLOCK;
  }

  CheckSystemCall( "fcntl F_SETFL", fcntl( fd_, F_SETFL, flags ) );
}

uint64_t FileDescriptor::seek(const int64_t offset, const int whence)
{
  return CheckSystemCall("lseek", lseek(fd_num(), offset, whence));
}

uint64_t FileDescriptor::curr_offset()
{
  return seek(0, SEEK_CUR);
}

uint64_t FileDescriptor::inc_offset(const int64_t offset)
{
  return seek(offset, SEEK_CUR);
}

uint64_t FileDescriptor::filesize()
{
  uint64_t prev_offset = curr_offset();
  uint64_t fsize = seek(0, SEEK_END);

  /* seek back to the previous offset */
  seek(prev_offset, SEEK_SET);

  return fsize;
}

void FileDescriptor::reset()
{
  seek(0, SEEK_SET);
  set_eof(false);
}

void FileDescriptor::attach_epoller(shared_ptr<Epoller> && epoller_ptr)
{
  auto ret = epollers_.emplace(epoller_ptr->fd_num(), move(epoller_ptr));

  if (not ret.second) {
    cerr << "Warning: failed to attach an already-existent epoller "
         << epoller_ptr->fd_num() << " to FileDescriptor" << endl;
  }
}

void FileDescriptor::detach_epoller(const int epoller_fd)
{
  if (epollers_.erase(epoller_fd) == 0) {
    cerr << "Warning: failed to detach a non-existent epoller "
         << epoller_fd << " from FileDescriptor" << endl;
  }
}
