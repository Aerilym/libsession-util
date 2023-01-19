#pragma once

#include <cstddef>
#include <chrono>
#include <iterator>
#include <memory>
#include <session/config.hpp>

#include "base.hpp"

extern "C" struct convo_info;

namespace session::config {

class Conversations;

/// keys used in this config, either currently or in the past (so that we don't reuse):
///
/// 1 - dict of one-to-one conversations.  Each key is the Session ID of the contact (in hex).
///     Values are dicts with keys:
///     r - the unix timestamp (in integer milliseconds) of the last-read message.  Always
///         included, but will be 0 if no messages are read.
///     e - Disappearing messages expiration type.  Omitted if disappearing messages are not enabled
///         for this conversation, 1 for delete-after-send, and 2 for delete-after-read.
///     E - Disappearing message timer, in minutes.  Omitted when `e` is omitted.
///
/// o - open group conversations.  Each key is: BASE_URL + '\0' + LC_ROOM_NAME + '\0' +
///     SERVER_PUBKEY (in bytes).  Note that room name is *always* lower-cased here (so that clients
///     with the same room but with different cases will always set the same key).  Values are dicts
///     with keys:
///     r - the unix timestamp (in integer milliseconds) of the last-read message.  Always included,
///         but will be 0 if no messages are read.
///
/// C - legacy closed group conversations.  The key is the closed group identifier (which looks
///     indistinguishable from a Session ID, but isn't really a proper Session ID).  Values are
///     dicts with keys:
///     r - the unix timestamp (integer milliseconds) of the last-read message.  Always included,
///         but will be 0 if no messages are read.
///
/// c - reserved for future tracking of new closed group conversations.



namespace convo {

    enum class expiration_mode : int8_t {
        none,
        after_send,
        after_read
    };

    struct one_to_one {
        std::string session_id; // in hex
        int64_t last_read = 0;
        expiration_mode expiration = expiration_mode::none;
        std::chrono::minutes expiration_timer{0};

        // Constructs an empty one_to_one from a session_id
        explicit one_to_one(std::string&& session_id);
        explicit one_to_one(std::string_view session_id);

        private:
        friend class session::config::Conversations;
        void load(const dict& info_dict);
    };

    struct open_group {
        std::string_view base_url();  // Accesses the base url (i.e. not including room or pubkey).
                                      // Always lower-case.
        std::string_view room(); // Accesses the room name, always in lower-case.  (Note that the
                                 // actual open group info might not be lower-case; it is just in
                                 // the open group convo where we force it lower-case).
        ustring_view pubkey(); // Accesses the server pubkey (32 bytes).
        std::string pubkey_hex(); // Accesses the server pubkey as hex (64 hex digits).

        int64_t last_read = 0;

        open_group() = default;

        // Constructs an empty open_group convo struct from url, room, and pubkey.  `base_url` and
        // `room` will be lower-cased if not already (they do not have to be passed lower-case).
        // pubkey is 32 bytes.
        open_group(std::string_view base_url, std::string_view room, ustring_view pubkey);

        // Same as above, but takes pubkey as a hex string.
        open_group(std::string_view base_url, std::string_view room, std::string_view pubkey_hex);

        // Replaces the baseurl/room/pubkey of this object.
        void set_server(std::string_view base_url, std::string_view room, ustring_view pubkey);
        void set_server(std::string_view base_url, std::string_view room, std::string_view pubkey_hex);

        // Loads the baseurl/room/pubkey of this object from an encoded key.  Throws
        // std::invalid_argument if the encoded key does not look right.
        void load_encoded_key(std::string key);

        private:
            std::string key;
            size_t url_size = 0;

        friend class session::config::Conversations;

        void load(const dict& info_dict);

        // Returns the key value we use in the stored dict for this open group, i.e.
        // lc(URL) + lc(NAME) + PUBKEY_BYTES.
        static std::string make_key(std::string_view base_url, std::string_view room, std::string_view pubkey_hex);
        static std::string make_key(std::string_view base_url, std::string_view room, ustring_view pubkey);
    };

    struct legacy_closed_group {
        std::string id; // in hex, indistinguishable from a Session ID
        int64_t last_read = 0;

        // Constructs an empty legacy_closed_group from a quasi-session_id
        explicit legacy_closed_group(std::string&& group_id);
        explicit legacy_closed_group(std::string_view group_id);

        private:
        friend class session::config::Conversations;
        void load(const dict& info_dict);
    };

    using any = std::variant<one_to_one, open_group, legacy_closed_group>;
}

class Conversations : public ConfigBase {

  public:
    // No default constructor
    Conversations() = delete;

    /// Constructs a conversation list from existing data (stored from `dump()`) and the user's
    /// secret key for generating the data encryption key.  To construct a blank list (i.e. with no
    /// pre-existing dumped data to load) pass `std::nullopt` as the second argument.
    ///
    /// \param ed25519_secretkey - contains the libsodium secret key used to encrypt/decrypt the
    /// data when pushing/pulling from the swarm.  This can either be the full 64-byte value (which
    /// is technically the 32-byte seed followed by the 32-byte pubkey), or just the 32-byte seed of
    /// the secret key.
    ///
    /// \param dumped - either `std::nullopt` to construct a new, empty object; or binary state data
    /// that was previously dumped from an instance of this class by calling `dump()`.
    Conversations(ustring_view ed25519_secretkey, std::optional<ustring_view> dumped);

    Namespace storage_namespace() const override { return Namespace::Conversations; }

    const char* encryption_domain() const override { return "Conversations"; }

    /// Looks up and returns a contact by session ID (hex).  Returns nullopt if the session ID was
    /// not found, otherwise returns a filled out `convo::one_to_one`.
    std::optional<convo::one_to_one> get_1to1(std::string_view session_id) const;

    /// Looks up and returns an open group conversation.  Takes the base URL, room name (case
    /// insensitive), and pubkey (in hex).  Retuns nullopt if the open group was not found,
    /// otherwise a filled out `convo::open_group`.
    std::optional<convo::open_group> get_open(std::string_view base_url,
            std::string_view room,
            std::string_view pubkey_hex) const;

    /// Same as above, but takes the pubkey as bytes instead of hex
    std::optional<convo::open_group> get_open(std::string_view base_url,
            std::string_view room,
            ustring_view pubkey) const;

    /// Looks up and returns a legacy closed group conversation by ID.  The ID looks like a hex
    /// Session ID, but isn't really a Session ID.  Returns nullopt if there is no record of the
    /// closed group conversation.
    std::optional<convo::legacy_closed_group> get_legacy_closed(std::string_view pubkey_hex) const;

    /// These are the same as the above methods (without "_or_construct" in the name), except that
    /// when the conversation doesn't exist a new one is created, prefilled with the pubkey/url/etc.
    convo::one_to_one get_or_construct_1to1(std::string_view session_id) const;
    convo::open_group get_or_construct_open(std::string_view base_url,
            std::string_view room,
            std::string_view pubkey_hex) const;
    convo::open_group get_or_construct_open(std::string_view base_url,
            std::string_view room,
            ustring_view pubkey) const;
    convo::legacy_closed_group get_or_construct_legacy_closed(std::string_view pubkey_hex) const;


    /// Inserts or replaces existing conversation info.  For example, to update a 1-to-1
    /// conversation last read time you would do:
    ///
    ///     auto info = conversations.get_or_construct_1to1(some_session_id);
    ///     info.last_read = new_unix_timestamp;
    ///     conversations.set(info);
    ///
    void set(const convo::one_to_one& c);
    void set(const convo::legacy_closed_group& c);
    void set(const convo::open_group& c);

    void set(const convo::any& c); // Variant which can be any of the above

    /// Removes a one-to-one conversation.  Returns true if found and removed, false if not present.
    bool erase_1to1(std::string_view pubkey);

    /// Removes an open group conversation record.  Returns true if found and removed, false if not
    /// present.  Arguments are the same as `get_open`.
    bool erase_open(std::string_view base_url, std::string_view room, std::string_view pubkey_hex);

    /// Removes a legacy closed group conversation.  Returns true if found and removed, false if not
    /// present.
    bool erase_legacy_closed(std::string_view pubkey_hex);

    /// Removes a conversation taking the convo::whatever record (rather than the pubkey/url).
    bool erase(const convo::one_to_one& c);
    bool erase(const convo::open_group& c);
    bool erase(const convo::legacy_closed_group& c);

    bool erase(const convo::any& c); // Variant of any of them

    struct iterator;

    /// This works like erase, but takes an iterator to the conversation to remove.  The element is
    /// removed and the iterator to the next element after the removed one is returned.  This is
    /// intended for use where elements are to be removed during iteration: see below for an
    /// example.
    iterator erase(iterator it);

    /// Returns the number of conversations (of any type).
    size_t size() const;

    /// Returns the number of 1-to-1, open group, and legacy closed group conversations,
    /// respectively.
    size_t size_1to1() const;
    size_t size_open() const;
    size_t size_legacy_closed() const;

    /// Returns true if the conversation list is empty.
    bool empty() const { return size() == 0; }

    /// Iterators for iterating through all conversations.  Typically you access this implicit via a for
    /// loop over the `Conversations` object:
    ///
    ///     for (auto& convo : conversations) {
    ///         if (auto* dm = std::get_if<convo::one_to_one>(&convo)) {
    ///             // use dm->session_id, dm->last_read, etc.
    ///         } else if (auto* og = std::get_if<convo::open_group>(&convo)) {
    ///             // use og->base_url, og->room, om->last_read, etc.
    ///         } else if (auto* lcg = std::get_if<convo::legacy_closed_group>(&convo)) {
    ///             // use lcg->id, lcg->last_read
    ///         }
    ///     }
    ///
    /// This iterates through all conversations in sorted order (sorted first by convo type, then by
    /// id within the type).
    ///
    /// It is permitted to modify and add records while iterating (e.g. by modifying one of the
    /// `dm`/`og`/`lcg` and then calling set()).
    ///
    /// If you need to erase the current conversation during iteration then care is required: you
    /// need to advance the iterator via the iterator version of erase when erasing an element
    /// rather than incrementing it regularly.  For example:
    ///
    ///     for (auto it = conversations.begin(); it != conversations.end(); ) {
    ///         if (should_remove(*it))
    ///             it = converations.erase(it);
    ///         else
    ///             ++it;
    ///     }
    ///
    /// Alternatively, you can use the first version with two loops: the first loop through all
    /// converations doesn't erase but just builds a vector of IDs to erase, then the second loops
    /// through that vector calling `erase_1to1()`/`erase_open()`/`erase_legacy_closed()` for each
    /// one.
    ///
    iterator begin() const;
    iterator end() const { return iterator{}; }

    using iterator_category = std::input_iterator_tag;
    using value_type = std::variant<convo::one_to_one, convo::open_group, convo::legacy_closed_group>;
    using reference = value_type&;
    using pointer = value_type*;
    using difference_type = std::ptrdiff_t;

    struct iterator {
      private:
        std::shared_ptr<convo::any> _val;
        std::optional<dict::const_iterator> _it_11, _end_11, _it_open, _end_open, _it_lclosed, _end_lclosed;
        void _load_val();
        iterator() = default; // Constructs an end tombstone
        explicit iterator(const DictFieldRoot& data);
        friend class Conversations;

      public:
        bool operator==(const iterator& other) const;
        bool operator!=(const iterator& other) const { return !(*this == other); }
        bool done() const;  // Equivalent to comparing against the end iterator
        convo::any& operator*() const { return *_val; }
        convo::any* operator->() const { return _val.get(); }
        iterator& operator++();
        iterator operator++(int) {
            auto copy{*this};
            ++*this;
            return copy;
        }
    };
};

}  // namespace session::config
