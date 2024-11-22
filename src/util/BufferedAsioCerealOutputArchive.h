#pragma once

#include "util/XDRStream.h"
#include <cereal/archives/binary.hpp>
#include <cereal/cereal.hpp>
#include <cereal/types/string.hpp>

namespace cereal
{

// Mirrors CEREAL_ARCHIVE_RESTRICT from cereal/details/traits.hpp for single
// types
#define CEREAL_ARCHIVE_RESTRICT_SINGLE_TYPE(TYPE) \
    typename std::enable_if< \
        cereal::traits::is_same_archive<Archive, TYPE>::value, void>::type

// This is a basic reimplementation of BinaryOutputArchive
// (cereal/archives/binary.hpp) that uses our own OutputFileStream instead of
// std::ofstream for writes in order to support fsync. For input we can just use
// cereal's BinaryInputArchive because we don't care about fsync for reads.
class BufferedAsioOutputArchive
    : public OutputArchive<BufferedAsioOutputArchive, AllowEmptyClassElision>
{
  public:
    // Construct, outputting to the provided stream
    // @param stream The stream to output to.  Can be a stringstream, a file
    //               stream, or even cout!
    BufferedAsioOutputArchive(stellar::OutputFileStream& stream)
        : OutputArchive<BufferedAsioOutputArchive, AllowEmptyClassElision>(this)
        , itsStream(stream)
    {
    }

    ~BufferedAsioOutputArchive() CEREAL_NOEXCEPT = default;

    // Writes size bytes of data to the output stream
    void
    saveBinary(const void* data, std::streamsize size)
    {
        itsStream.writeBytes(static_cast<char const*>(data), size);
    }

  private:
    stellar::OutputFileStream& itsStream;
};

// Saving for POD types to binary
template <class T>
inline typename std::enable_if<std::is_arithmetic<T>::value, void>::type
CEREAL_SAVE_FUNCTION_NAME(BufferedAsioOutputArchive& ar, T const& t)
{
    ar.saveBinary(std::addressof(t), sizeof(t));
}

// Serializing NVP types to binary
template <class Archive, class T>

inline CEREAL_ARCHIVE_RESTRICT_SINGLE_TYPE(BufferedAsioOutputArchive)
    CEREAL_SERIALIZE_FUNCTION_NAME(Archive& ar, NameValuePair<T>& t)
{
    ar(t.value);
}

// Serializing SizeTags to binary
template <class Archive, class T>
inline CEREAL_ARCHIVE_RESTRICT_SINGLE_TYPE(BufferedAsioOutputArchive)
    CEREAL_SERIALIZE_FUNCTION_NAME(Archive& ar, SizeTag<T>& t)
{
    ar(t.size);
}

// Saving binary data
template <class T>
inline void
CEREAL_SAVE_FUNCTION_NAME(BufferedAsioOutputArchive& ar,
                          BinaryData<T> const& bd)
{
    ar.saveBinary(bd.data, static_cast<std::streamsize>(bd.size));
}
}

CEREAL_REGISTER_ARCHIVE(cereal::BufferedAsioOutputArchive)
