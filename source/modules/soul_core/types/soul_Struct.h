/*
    _____ _____ _____ __
   |   __|     |  |  |  |      The SOUL language
   |__   |  |  |  |  |  |__    Copyright (c) 2019 - ROLI Ltd.
   |_____|_____|_____|_____|

   The code in this file is provided under the terms of the ISC license:

   Permission to use, copy, modify, and/or distribute this software for any purpose
   with or without fee is hereby granted, provided that the above copyright notice and
   this permission notice appear in all copies.

   THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES WITH REGARD
   TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS. IN
   NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL
   DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER
   IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
   CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
*/

namespace soul
{

//==============================================================================
/** Represents a structure.
    @see Type::createStruct()
*/
class Structure   : public RefCountedObject
{
public:
    Structure (std::string name);

    Structure (const Structure&) = default;
    Structure (Structure&&) = default;
    Structure& operator= (const Structure&) = delete;
    Structure& operator= (Structure&&) = delete;

    struct Member
    {
        Type type;
        std::string name;
    };

    std::string name;
    ArrayWithPreallocation<Member, 8> members;

    size_t addMember (Type, std::string memberName);
    bool hasMemberWithName (const std::string& memberName) const;
    size_t getMemberIndex (const std::string& memberName) const;
    size_t addMemberWithUniqueName (Type, const std::string& memberName);

    bool isEmpty() const noexcept;
    size_t getPackedSizeInBytes() const;

    void checkForRecursiveNestedStructs (const CodeLocation&);
};


} // namespace soul
