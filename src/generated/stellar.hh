// -*- C++ -*-
// Automatically generated from src/overlay/protocol.x.
// DO NOT EDIT or your changes may be overwritten

#ifndef __XDR_PROTOCOL_HH_INCLUDED__
#define __XDR_PROTOCOL_HH_INCLUDED__ 1

#include <xdrpp/types.h>

namespace stellarxdr {

using uint256 = xdr::opaque_array<32>;
using uint160 = xdr::opaque_array<20>;
using uint64 = std::uint64_t;
using uint32 = std::uint32_t;

struct Error {
  std::int32_t code{};
  xdr::xstring<100> msg{};
  
  Error() = default;
  template<typename _code_T,
           typename _msg_T>
  explicit Error(_code_T &&_code,
                 _msg_T &&_msg)
    : code(std::forward<_code_T>(_code)),
      msg(std::forward<_msg_T>(_msg)) {}
};
} namespace xdr {
template<> struct xdr_traits<::stellarxdr::Error>
  : xdr_struct_base<field_ptr<::stellarxdr::Error,
                              decltype(::stellarxdr::Error::code),
                              &::stellarxdr::Error::code>,
                    field_ptr<::stellarxdr::Error,
                              decltype(::stellarxdr::Error::msg),
                              &::stellarxdr::Error::msg>> {
  template<typename Archive> static void
  save(Archive &ar, const ::stellarxdr::Error &obj) {
    archive(ar, obj.code, "code");
    archive(ar, obj.msg, "msg");
  }
  template<typename Archive> static void
  load(Archive &ar, ::stellarxdr::Error &obj) {
    archive(ar, obj.code, "code");
    archive(ar, obj.msg, "msg");
  }
};
} namespace stellarxdr {

struct Hello {
  std::int32_t protocolVersion{};
  xdr::xstring<100> versionStr{};
  std::int32_t port{};
  
  Hello() = default;
  template<typename _protocolVersion_T,
           typename _versionStr_T,
           typename _port_T>
  explicit Hello(_protocolVersion_T &&_protocolVersion,
                 _versionStr_T &&_versionStr,
                 _port_T &&_port)
    : protocolVersion(std::forward<_protocolVersion_T>(_protocolVersion)),
      versionStr(std::forward<_versionStr_T>(_versionStr)),
      port(std::forward<_port_T>(_port)) {}
};
} namespace xdr {
template<> struct xdr_traits<::stellarxdr::Hello>
  : xdr_struct_base<field_ptr<::stellarxdr::Hello,
                              decltype(::stellarxdr::Hello::protocolVersion),
                              &::stellarxdr::Hello::protocolVersion>,
                    field_ptr<::stellarxdr::Hello,
                              decltype(::stellarxdr::Hello::versionStr),
                              &::stellarxdr::Hello::versionStr>,
                    field_ptr<::stellarxdr::Hello,
                              decltype(::stellarxdr::Hello::port),
                              &::stellarxdr::Hello::port>> {
  template<typename Archive> static void
  save(Archive &ar, const ::stellarxdr::Hello &obj) {
    archive(ar, obj.protocolVersion, "protocolVersion");
    archive(ar, obj.versionStr, "versionStr");
    archive(ar, obj.port, "port");
  }
  template<typename Archive> static void
  load(Archive &ar, ::stellarxdr::Hello &obj) {
    archive(ar, obj.protocolVersion, "protocolVersion");
    archive(ar, obj.versionStr, "versionStr");
    archive(ar, obj.port, "port");
  }
};
} namespace stellarxdr {

struct Transaction {
  uint160 account{};
  
  Transaction() = default;
  template<typename _account_T>
  explicit Transaction(_account_T &&_account)
    : account(std::forward<_account_T>(_account)) {}
};
} namespace xdr {
template<> struct xdr_traits<::stellarxdr::Transaction>
  : xdr_struct_base<field_ptr<::stellarxdr::Transaction,
                              decltype(::stellarxdr::Transaction::account),
                              &::stellarxdr::Transaction::account>> {
  template<typename Archive> static void
  save(Archive &ar, const ::stellarxdr::Transaction &obj) {
    archive(ar, obj.account, "account");
  }
  template<typename Archive> static void
  load(Archive &ar, ::stellarxdr::Transaction &obj) {
    archive(ar, obj.account, "account");
  }
};
} namespace stellarxdr {

struct TransactionEnvelope {
  Transaction tx{};
  uint256 signature{};
  
  TransactionEnvelope() = default;
  template<typename _tx_T,
           typename _signature_T>
  explicit TransactionEnvelope(_tx_T &&_tx,
                               _signature_T &&_signature)
    : tx(std::forward<_tx_T>(_tx)),
      signature(std::forward<_signature_T>(_signature)) {}
};
} namespace xdr {
template<> struct xdr_traits<::stellarxdr::TransactionEnvelope>
  : xdr_struct_base<field_ptr<::stellarxdr::TransactionEnvelope,
                              decltype(::stellarxdr::TransactionEnvelope::tx),
                              &::stellarxdr::TransactionEnvelope::tx>,
                    field_ptr<::stellarxdr::TransactionEnvelope,
                              decltype(::stellarxdr::TransactionEnvelope::signature),
                              &::stellarxdr::TransactionEnvelope::signature>> {
  template<typename Archive> static void
  save(Archive &ar, const ::stellarxdr::TransactionEnvelope &obj) {
    archive(ar, obj.tx, "tx");
    archive(ar, obj.signature, "signature");
  }
  template<typename Archive> static void
  load(Archive &ar, ::stellarxdr::TransactionEnvelope &obj) {
    archive(ar, obj.tx, "tx");
    archive(ar, obj.signature, "signature");
  }
};
} namespace stellarxdr {

struct TransactionSet {
  xdr::xvector<TransactionEnvelope> txs{};
  
  TransactionSet() = default;
  template<typename _txs_T>
  explicit TransactionSet(_txs_T &&_txs)
    : txs(std::forward<_txs_T>(_txs)) {}
};
} namespace xdr {
template<> struct xdr_traits<::stellarxdr::TransactionSet>
  : xdr_struct_base<field_ptr<::stellarxdr::TransactionSet,
                              decltype(::stellarxdr::TransactionSet::txs),
                              &::stellarxdr::TransactionSet::txs>> {
  template<typename Archive> static void
  save(Archive &ar, const ::stellarxdr::TransactionSet &obj) {
    archive(ar, obj.txs, "txs");
  }
  template<typename Archive> static void
  load(Archive &ar, ::stellarxdr::TransactionSet &obj) {
    archive(ar, obj.txs, "txs");
  }
};
} namespace stellarxdr {

struct LedgerHeader {
  uint256 previousLedgerHash{};
  uint256 txSetHash{};
  
  LedgerHeader() = default;
  template<typename _previousLedgerHash_T,
           typename _txSetHash_T>
  explicit LedgerHeader(_previousLedgerHash_T &&_previousLedgerHash,
                        _txSetHash_T &&_txSetHash)
    : previousLedgerHash(std::forward<_previousLedgerHash_T>(_previousLedgerHash)),
      txSetHash(std::forward<_txSetHash_T>(_txSetHash)) {}
};
} namespace xdr {
template<> struct xdr_traits<::stellarxdr::LedgerHeader>
  : xdr_struct_base<field_ptr<::stellarxdr::LedgerHeader,
                              decltype(::stellarxdr::LedgerHeader::previousLedgerHash),
                              &::stellarxdr::LedgerHeader::previousLedgerHash>,
                    field_ptr<::stellarxdr::LedgerHeader,
                              decltype(::stellarxdr::LedgerHeader::txSetHash),
                              &::stellarxdr::LedgerHeader::txSetHash>> {
  template<typename Archive> static void
  save(Archive &ar, const ::stellarxdr::LedgerHeader &obj) {
    archive(ar, obj.previousLedgerHash, "previousLedgerHash");
    archive(ar, obj.txSetHash, "txSetHash");
  }
  template<typename Archive> static void
  load(Archive &ar, ::stellarxdr::LedgerHeader &obj) {
    archive(ar, obj.previousLedgerHash, "previousLedgerHash");
    archive(ar, obj.txSetHash, "txSetHash");
  }
};
} namespace stellarxdr {

struct LedgerHistory {
  LedgerHeader header{};
  TransactionSet txSet{};
  
  LedgerHistory() = default;
  template<typename _header_T,
           typename _txSet_T>
  explicit LedgerHistory(_header_T &&_header,
                         _txSet_T &&_txSet)
    : header(std::forward<_header_T>(_header)),
      txSet(std::forward<_txSet_T>(_txSet)) {}
};
} namespace xdr {
template<> struct xdr_traits<::stellarxdr::LedgerHistory>
  : xdr_struct_base<field_ptr<::stellarxdr::LedgerHistory,
                              decltype(::stellarxdr::LedgerHistory::header),
                              &::stellarxdr::LedgerHistory::header>,
                    field_ptr<::stellarxdr::LedgerHistory,
                              decltype(::stellarxdr::LedgerHistory::txSet),
                              &::stellarxdr::LedgerHistory::txSet>> {
  template<typename Archive> static void
  save(Archive &ar, const ::stellarxdr::LedgerHistory &obj) {
    archive(ar, obj.header, "header");
    archive(ar, obj.txSet, "txSet");
  }
  template<typename Archive> static void
  load(Archive &ar, ::stellarxdr::LedgerHistory &obj) {
    archive(ar, obj.header, "header");
    archive(ar, obj.txSet, "txSet");
  }
};
} namespace stellarxdr {

struct History {
  std::int32_t fromLedger{};
  std::int32_t toLedger{};
  xdr::xvector<LedgerHistory> ledgers{};
  
  History() = default;
  template<typename _fromLedger_T,
           typename _toLedger_T,
           typename _ledgers_T>
  explicit History(_fromLedger_T &&_fromLedger,
                   _toLedger_T &&_toLedger,
                   _ledgers_T &&_ledgers)
    : fromLedger(std::forward<_fromLedger_T>(_fromLedger)),
      toLedger(std::forward<_toLedger_T>(_toLedger)),
      ledgers(std::forward<_ledgers_T>(_ledgers)) {}
};
} namespace xdr {
template<> struct xdr_traits<::stellarxdr::History>
  : xdr_struct_base<field_ptr<::stellarxdr::History,
                              decltype(::stellarxdr::History::fromLedger),
                              &::stellarxdr::History::fromLedger>,
                    field_ptr<::stellarxdr::History,
                              decltype(::stellarxdr::History::toLedger),
                              &::stellarxdr::History::toLedger>,
                    field_ptr<::stellarxdr::History,
                              decltype(::stellarxdr::History::ledgers),
                              &::stellarxdr::History::ledgers>> {
  template<typename Archive> static void
  save(Archive &ar, const ::stellarxdr::History &obj) {
    archive(ar, obj.fromLedger, "fromLedger");
    archive(ar, obj.toLedger, "toLedger");
    archive(ar, obj.ledgers, "ledgers");
  }
  template<typename Archive> static void
  load(Archive &ar, ::stellarxdr::History &obj) {
    archive(ar, obj.fromLedger, "fromLedger");
    archive(ar, obj.toLedger, "toLedger");
    archive(ar, obj.ledgers, "ledgers");
  }
};
} namespace stellarxdr {

struct Ballot {
  std::int32_t index{};
  uint256 txSetHash{};
  uint64 closeTime{};
  
  Ballot() = default;
  template<typename _index_T,
           typename _txSetHash_T,
           typename _closeTime_T>
  explicit Ballot(_index_T &&_index,
                  _txSetHash_T &&_txSetHash,
                  _closeTime_T &&_closeTime)
    : index(std::forward<_index_T>(_index)),
      txSetHash(std::forward<_txSetHash_T>(_txSetHash)),
      closeTime(std::forward<_closeTime_T>(_closeTime)) {}
};
} namespace xdr {
template<> struct xdr_traits<::stellarxdr::Ballot>
  : xdr_struct_base<field_ptr<::stellarxdr::Ballot,
                              decltype(::stellarxdr::Ballot::index),
                              &::stellarxdr::Ballot::index>,
                    field_ptr<::stellarxdr::Ballot,
                              decltype(::stellarxdr::Ballot::txSetHash),
                              &::stellarxdr::Ballot::txSetHash>,
                    field_ptr<::stellarxdr::Ballot,
                              decltype(::stellarxdr::Ballot::closeTime),
                              &::stellarxdr::Ballot::closeTime>> {
  template<typename Archive> static void
  save(Archive &ar, const ::stellarxdr::Ballot &obj) {
    archive(ar, obj.index, "index");
    archive(ar, obj.txSetHash, "txSetHash");
    archive(ar, obj.closeTime, "closeTime");
  }
  template<typename Archive> static void
  load(Archive &ar, ::stellarxdr::Ballot &obj) {
    archive(ar, obj.index, "index");
    archive(ar, obj.txSetHash, "txSetHash");
    archive(ar, obj.closeTime, "closeTime");
  }
};
} namespace stellarxdr {

struct SlotBallot {
  std::int32_t ledgerIndex{};
  uint256 previousLedgerHash{};
  Ballot ballot{};
  
  SlotBallot() = default;
  template<typename _ledgerIndex_T,
           typename _previousLedgerHash_T,
           typename _ballot_T>
  explicit SlotBallot(_ledgerIndex_T &&_ledgerIndex,
                      _previousLedgerHash_T &&_previousLedgerHash,
                      _ballot_T &&_ballot)
    : ledgerIndex(std::forward<_ledgerIndex_T>(_ledgerIndex)),
      previousLedgerHash(std::forward<_previousLedgerHash_T>(_previousLedgerHash)),
      ballot(std::forward<_ballot_T>(_ballot)) {}
};
} namespace xdr {
template<> struct xdr_traits<::stellarxdr::SlotBallot>
  : xdr_struct_base<field_ptr<::stellarxdr::SlotBallot,
                              decltype(::stellarxdr::SlotBallot::ledgerIndex),
                              &::stellarxdr::SlotBallot::ledgerIndex>,
                    field_ptr<::stellarxdr::SlotBallot,
                              decltype(::stellarxdr::SlotBallot::previousLedgerHash),
                              &::stellarxdr::SlotBallot::previousLedgerHash>,
                    field_ptr<::stellarxdr::SlotBallot,
                              decltype(::stellarxdr::SlotBallot::ballot),
                              &::stellarxdr::SlotBallot::ballot>> {
  template<typename Archive> static void
  save(Archive &ar, const ::stellarxdr::SlotBallot &obj) {
    archive(ar, obj.ledgerIndex, "ledgerIndex");
    archive(ar, obj.previousLedgerHash, "previousLedgerHash");
    archive(ar, obj.ballot, "ballot");
  }
  template<typename Archive> static void
  load(Archive &ar, ::stellarxdr::SlotBallot &obj) {
    archive(ar, obj.ledgerIndex, "ledgerIndex");
    archive(ar, obj.previousLedgerHash, "previousLedgerHash");
    archive(ar, obj.ballot, "ballot");
  }
};
} namespace stellarxdr {

enum FBAStatementType : std::uint32_t {
  PREPARE,
  PREPARED,
  COMMIT,
  COMMITTED,
};
} namespace xdr {
template<> struct xdr_traits<::stellarxdr::FBAStatementType>
  : xdr_integral_base<::stellarxdr::FBAStatementType, std::uint32_t> {
  static const bool is_enum = true;
  static const bool is_numeric = false;
  static const char *enum_name(::stellarxdr::FBAStatementType val) {
    switch (val) {
    case ::stellarxdr::PREPARE:
      return "PREPARE";
    case ::stellarxdr::PREPARED:
      return "PREPARED";
    case ::stellarxdr::COMMIT:
      return "COMMIT";
    case ::stellarxdr::COMMITTED:
      return "COMMITTED";
    default:
      return nullptr;
    }
  }
  static const std::vector<uint32_t> &enum_values() {
    static const std::vector<uint32_t> _xdr_enum_vec = {
      ::stellarxdr::PREPARE,
      ::stellarxdr::PREPARED,
      ::stellarxdr::COMMIT,
      ::stellarxdr::COMMITTED
    };
    return _xdr_enum_vec;
  }
};
} namespace stellarxdr {

struct FBAContents {
  struct _body_t {
  private:
    std::uint32_t type_;
    union {
      xdr::xvector<Ballot> excepted_;
    };

  public:
    static_assert (sizeof (FBAStatementType) <= 4, "union discriminant must be 4 bytes");

    static int _xdr_field_number(std::uint32_t which) {
      return which == PREPARE ? 1
        : which == PREPARED || which == COMMIT || which == COMMITTED ? 0
        : -1;
    }
    template<typename _F, typename...A> static bool
    _xdr_with_mem_ptr(_F &_f, std::uint32_t which, A&&...a) {
      switch (which) {
      case PREPARE:
        _f(&_body_t::excepted_, std::forward<A>(a)...);
        return true;
      case PREPARED:
      case COMMIT:
      case COMMITTED:
        return true;
      }
      return false;
    }

    std::uint32_t _xdr_discriminant() const { return type_; }
    void _xdr_discriminant(std::uint32_t which, bool validate = true) {
      int fnum = _xdr_field_number(which);
      if (fnum < 0 && validate)
        throw xdr::xdr_bad_discriminant("bad value of type in _body_t");
      if (fnum != _xdr_field_number(type_)) {
        this->~_body_t();
        type_ = which;
        _xdr_with_mem_ptr(xdr::field_constructor, type_, *this);
      }
    }
    _body_t(FBAStatementType which = FBAStatementType{}) : type_(which) {
      _xdr_with_mem_ptr(xdr::field_constructor, type_, *this);
    }
    _body_t(const _body_t &source) : type_(source.type_) {
      _xdr_with_mem_ptr(xdr::field_constructor, type_, *this, source);
    }
    _body_t(_body_t &&source) : type_(source.type_) {
      _xdr_with_mem_ptr(xdr::field_constructor, type_, *this,
                        std::move(source));
    }
    ~_body_t() { _xdr_with_mem_ptr(xdr::field_destructor, type_, *this); }
    _body_t &operator=(const _body_t &source) {
      if (_xdr_field_number(type_) 
          == _xdr_field_number(source.type_))
        _xdr_with_mem_ptr(xdr::field_assigner, type_, *this, source);
      else {
        this->~_body_t();
        type_ = std::uint32_t(-1);
        _xdr_with_mem_ptr(xdr::field_constructor, type_, *this, source);
      }
      type_ = source.type_;
      return *this;
    }
    _body_t &operator=(_body_t &&source) {
      if (_xdr_field_number(type_)
           == _xdr_field_number(source.type_))
        _xdr_with_mem_ptr(xdr::field_assigner, type_, *this,
                          std::move(source));
      else {
        this->~_body_t();
        type_ = std::uint32_t(-1);
        _xdr_with_mem_ptr(xdr::field_constructor, type_, *this,
                          std::move(source));
      }
      type_ = source.type_;
      return *this;
    }

    FBAStatementType type() const { return FBAStatementType(type_); }
    _body_t &type(FBAStatementType _xdr_d, bool _xdr_validate = true) {
      _xdr_discriminant(_xdr_d, _xdr_validate);
      return *this;
    }

    xdr::xvector<Ballot> &excepted() {
      if (_xdr_field_number(type_) == 1)
        return excepted_;
      throw xdr::xdr_wrong_union("_body_t: excepted accessed when not selected");
    }
    const xdr::xvector<Ballot> &excepted() const {
      if (_xdr_field_number(type_) == 1)
        return excepted_;
      throw xdr::xdr_wrong_union("_body_t: excepted accessed when not selected");
    }
  };

  SlotBallot ballot{};
  uint256 quorumSetHash{};
  _body_t body{};
  
  FBAContents() = default;
  template<typename _ballot_T,
           typename _quorumSetHash_T,
           typename _body_T>
  explicit FBAContents(_ballot_T &&_ballot,
                       _quorumSetHash_T &&_quorumSetHash,
                       _body_T &&_body)
    : ballot(std::forward<_ballot_T>(_ballot)),
      quorumSetHash(std::forward<_quorumSetHash_T>(_quorumSetHash)),
      body(std::forward<_body_T>(_body)) {}
};
} namespace xdr {
template<> struct xdr_traits<::stellarxdr::FBAContents::_body_t> : xdr_traits_base {
  static const bool is_class = true;
  static const bool is_union = true;
  static const bool has_fixed_size = false;

  using union_type = ::stellarxdr::FBAContents::_body_t;
  using discriminant_type = decltype(std::declval<union_type>().type());

  static const char *union_field_name(std::uint32_t which) {
    switch (union_type::_xdr_field_number(which)) {
    case 1:
      return "Ballot";
    }
    return nullptr;
  }
  static const char *union_field_name(const union_type &u) {
    return union_field_name(u._xdr_discriminant());
  }

  static std::size_t serial_size(const ::stellarxdr::FBAContents::_body_t &obj) {
    std::size_t size = 0;
    if (!obj._xdr_with_mem_ptr(field_size, obj._xdr_discriminant(), obj, size))
      throw xdr_bad_discriminant("bad value of type in _body_t");
    return size + 4;
  }
  template<typename Archive> static void
  save(Archive &ar, const ::stellarxdr::FBAContents::_body_t &obj) {
    xdr::archive(ar, obj.type(), "type");
    if (!obj._xdr_with_mem_ptr(field_archiver, obj.type(), ar, obj,
                               union_field_name(obj)))
      throw xdr_bad_discriminant("bad value of type in _body_t");
  }
  template<typename Archive> static void
  load(Archive &ar, ::stellarxdr::FBAContents::_body_t &obj) {
    discriminant_type which;
    xdr::archive(ar, which, "type");
    obj.type(which);
    obj._xdr_with_mem_ptr(field_archiver, obj.type(), ar, obj,
                          union_field_name(which));
  }
};
template<> struct xdr_traits<::stellarxdr::FBAContents>
  : xdr_struct_base<field_ptr<::stellarxdr::FBAContents,
                              decltype(::stellarxdr::FBAContents::ballot),
                              &::stellarxdr::FBAContents::ballot>,
                    field_ptr<::stellarxdr::FBAContents,
                              decltype(::stellarxdr::FBAContents::quorumSetHash),
                              &::stellarxdr::FBAContents::quorumSetHash>,
                    field_ptr<::stellarxdr::FBAContents,
                              decltype(::stellarxdr::FBAContents::body),
                              &::stellarxdr::FBAContents::body>> {
  template<typename Archive> static void
  save(Archive &ar, const ::stellarxdr::FBAContents &obj) {
    archive(ar, obj.ballot, "ballot");
    archive(ar, obj.quorumSetHash, "quorumSetHash");
    archive(ar, obj.body, "body");
  }
  template<typename Archive> static void
  load(Archive &ar, ::stellarxdr::FBAContents &obj) {
    archive(ar, obj.ballot, "ballot");
    archive(ar, obj.quorumSetHash, "quorumSetHash");
    archive(ar, obj.body, "body");
  }
};
} namespace stellarxdr {

struct FBAEnvelope {
  uint256 nodeID{};
  uint256 signature{};
  FBAContents contents{};
  
  FBAEnvelope() = default;
  template<typename _nodeID_T,
           typename _signature_T,
           typename _contents_T>
  explicit FBAEnvelope(_nodeID_T &&_nodeID,
                       _signature_T &&_signature,
                       _contents_T &&_contents)
    : nodeID(std::forward<_nodeID_T>(_nodeID)),
      signature(std::forward<_signature_T>(_signature)),
      contents(std::forward<_contents_T>(_contents)) {}
};
} namespace xdr {
template<> struct xdr_traits<::stellarxdr::FBAEnvelope>
  : xdr_struct_base<field_ptr<::stellarxdr::FBAEnvelope,
                              decltype(::stellarxdr::FBAEnvelope::nodeID),
                              &::stellarxdr::FBAEnvelope::nodeID>,
                    field_ptr<::stellarxdr::FBAEnvelope,
                              decltype(::stellarxdr::FBAEnvelope::signature),
                              &::stellarxdr::FBAEnvelope::signature>,
                    field_ptr<::stellarxdr::FBAEnvelope,
                              decltype(::stellarxdr::FBAEnvelope::contents),
                              &::stellarxdr::FBAEnvelope::contents>> {
  template<typename Archive> static void
  save(Archive &ar, const ::stellarxdr::FBAEnvelope &obj) {
    archive(ar, obj.nodeID, "nodeID");
    archive(ar, obj.signature, "signature");
    archive(ar, obj.contents, "contents");
  }
  template<typename Archive> static void
  load(Archive &ar, ::stellarxdr::FBAEnvelope &obj) {
    archive(ar, obj.nodeID, "nodeID");
    archive(ar, obj.signature, "signature");
    archive(ar, obj.contents, "contents");
  }
};
} namespace stellarxdr {

enum LedgerTypes : std::uint32_t {
  ACCOUNT,
  TRUSTLINE,
  OFFER,
};
} namespace xdr {
template<> struct xdr_traits<::stellarxdr::LedgerTypes>
  : xdr_integral_base<::stellarxdr::LedgerTypes, std::uint32_t> {
  static const bool is_enum = true;
  static const bool is_numeric = false;
  static const char *enum_name(::stellarxdr::LedgerTypes val) {
    switch (val) {
    case ::stellarxdr::ACCOUNT:
      return "ACCOUNT";
    case ::stellarxdr::TRUSTLINE:
      return "TRUSTLINE";
    case ::stellarxdr::OFFER:
      return "OFFER";
    default:
      return nullptr;
    }
  }
  static const std::vector<uint32_t> &enum_values() {
    static const std::vector<uint32_t> _xdr_enum_vec = {
      ::stellarxdr::ACCOUNT,
      ::stellarxdr::TRUSTLINE,
      ::stellarxdr::OFFER
    };
    return _xdr_enum_vec;
  }
};
} namespace stellarxdr {

struct Amount {
  uint64 value{};
  xdr::pointer<uint160> currency{};
  xdr::pointer<uint160> issuer{};
  
  Amount() = default;
  template<typename _value_T,
           typename _currency_T,
           typename _issuer_T>
  explicit Amount(_value_T &&_value,
                  _currency_T &&_currency,
                  _issuer_T &&_issuer)
    : value(std::forward<_value_T>(_value)),
      currency(std::forward<_currency_T>(_currency)),
      issuer(std::forward<_issuer_T>(_issuer)) {}
};
} namespace xdr {
template<> struct xdr_traits<::stellarxdr::Amount>
  : xdr_struct_base<field_ptr<::stellarxdr::Amount,
                              decltype(::stellarxdr::Amount::value),
                              &::stellarxdr::Amount::value>,
                    field_ptr<::stellarxdr::Amount,
                              decltype(::stellarxdr::Amount::currency),
                              &::stellarxdr::Amount::currency>,
                    field_ptr<::stellarxdr::Amount,
                              decltype(::stellarxdr::Amount::issuer),
                              &::stellarxdr::Amount::issuer>> {
  template<typename Archive> static void
  save(Archive &ar, const ::stellarxdr::Amount &obj) {
    archive(ar, obj.value, "value");
    archive(ar, obj.currency, "currency");
    archive(ar, obj.issuer, "issuer");
  }
  template<typename Archive> static void
  load(Archive &ar, ::stellarxdr::Amount &obj) {
    archive(ar, obj.value, "value");
    archive(ar, obj.currency, "currency");
    archive(ar, obj.issuer, "issuer");
  }
};
} namespace stellarxdr {

struct AccountEntry {
  uint160 accountID{};
  uint64 balance{};
  uint32 sequence{};
  uint32 ownerCount{};
  uint32 transferRate{};
  uint160 inflationDest{};
  uint256 pubKey{};
  std::int32_t flags{};
  
  AccountEntry() = default;
  template<typename _accountID_T,
           typename _balance_T,
           typename _sequence_T,
           typename _ownerCount_T,
           typename _transferRate_T,
           typename _inflationDest_T,
           typename _pubKey_T,
           typename _flags_T>
  explicit AccountEntry(_accountID_T &&_accountID,
                        _balance_T &&_balance,
                        _sequence_T &&_sequence,
                        _ownerCount_T &&_ownerCount,
                        _transferRate_T &&_transferRate,
                        _inflationDest_T &&_inflationDest,
                        _pubKey_T &&_pubKey,
                        _flags_T &&_flags)
    : accountID(std::forward<_accountID_T>(_accountID)),
      balance(std::forward<_balance_T>(_balance)),
      sequence(std::forward<_sequence_T>(_sequence)),
      ownerCount(std::forward<_ownerCount_T>(_ownerCount)),
      transferRate(std::forward<_transferRate_T>(_transferRate)),
      inflationDest(std::forward<_inflationDest_T>(_inflationDest)),
      pubKey(std::forward<_pubKey_T>(_pubKey)),
      flags(std::forward<_flags_T>(_flags)) {}
};
} namespace xdr {
template<> struct xdr_traits<::stellarxdr::AccountEntry>
  : xdr_struct_base<field_ptr<::stellarxdr::AccountEntry,
                              decltype(::stellarxdr::AccountEntry::accountID),
                              &::stellarxdr::AccountEntry::accountID>,
                    field_ptr<::stellarxdr::AccountEntry,
                              decltype(::stellarxdr::AccountEntry::balance),
                              &::stellarxdr::AccountEntry::balance>,
                    field_ptr<::stellarxdr::AccountEntry,
                              decltype(::stellarxdr::AccountEntry::sequence),
                              &::stellarxdr::AccountEntry::sequence>,
                    field_ptr<::stellarxdr::AccountEntry,
                              decltype(::stellarxdr::AccountEntry::ownerCount),
                              &::stellarxdr::AccountEntry::ownerCount>,
                    field_ptr<::stellarxdr::AccountEntry,
                              decltype(::stellarxdr::AccountEntry::transferRate),
                              &::stellarxdr::AccountEntry::transferRate>,
                    field_ptr<::stellarxdr::AccountEntry,
                              decltype(::stellarxdr::AccountEntry::inflationDest),
                              &::stellarxdr::AccountEntry::inflationDest>,
                    field_ptr<::stellarxdr::AccountEntry,
                              decltype(::stellarxdr::AccountEntry::pubKey),
                              &::stellarxdr::AccountEntry::pubKey>,
                    field_ptr<::stellarxdr::AccountEntry,
                              decltype(::stellarxdr::AccountEntry::flags),
                              &::stellarxdr::AccountEntry::flags>> {
  template<typename Archive> static void
  save(Archive &ar, const ::stellarxdr::AccountEntry &obj) {
    archive(ar, obj.accountID, "accountID");
    archive(ar, obj.balance, "balance");
    archive(ar, obj.sequence, "sequence");
    archive(ar, obj.ownerCount, "ownerCount");
    archive(ar, obj.transferRate, "transferRate");
    archive(ar, obj.inflationDest, "inflationDest");
    archive(ar, obj.pubKey, "pubKey");
    archive(ar, obj.flags, "flags");
  }
  template<typename Archive> static void
  load(Archive &ar, ::stellarxdr::AccountEntry &obj) {
    archive(ar, obj.accountID, "accountID");
    archive(ar, obj.balance, "balance");
    archive(ar, obj.sequence, "sequence");
    archive(ar, obj.ownerCount, "ownerCount");
    archive(ar, obj.transferRate, "transferRate");
    archive(ar, obj.inflationDest, "inflationDest");
    archive(ar, obj.pubKey, "pubKey");
    archive(ar, obj.flags, "flags");
  }
};
} namespace stellarxdr {

struct TrustLineEntry {
  uint160 lowAccount{};
  uint160 highAccount{};
  uint160 currency{};
  uint64 lowLimit{};
  uint64 highLimit{};
  uint64 balance{};
  bool lowAuthSet{};
  bool highAuthSet{};
  
  TrustLineEntry() = default;
  template<typename _lowAccount_T,
           typename _highAccount_T,
           typename _currency_T,
           typename _lowLimit_T,
           typename _highLimit_T,
           typename _balance_T,
           typename _lowAuthSet_T,
           typename _highAuthSet_T>
  explicit TrustLineEntry(_lowAccount_T &&_lowAccount,
                          _highAccount_T &&_highAccount,
                          _currency_T &&_currency,
                          _lowLimit_T &&_lowLimit,
                          _highLimit_T &&_highLimit,
                          _balance_T &&_balance,
                          _lowAuthSet_T &&_lowAuthSet,
                          _highAuthSet_T &&_highAuthSet)
    : lowAccount(std::forward<_lowAccount_T>(_lowAccount)),
      highAccount(std::forward<_highAccount_T>(_highAccount)),
      currency(std::forward<_currency_T>(_currency)),
      lowLimit(std::forward<_lowLimit_T>(_lowLimit)),
      highLimit(std::forward<_highLimit_T>(_highLimit)),
      balance(std::forward<_balance_T>(_balance)),
      lowAuthSet(std::forward<_lowAuthSet_T>(_lowAuthSet)),
      highAuthSet(std::forward<_highAuthSet_T>(_highAuthSet)) {}
};
} namespace xdr {
template<> struct xdr_traits<::stellarxdr::TrustLineEntry>
  : xdr_struct_base<field_ptr<::stellarxdr::TrustLineEntry,
                              decltype(::stellarxdr::TrustLineEntry::lowAccount),
                              &::stellarxdr::TrustLineEntry::lowAccount>,
                    field_ptr<::stellarxdr::TrustLineEntry,
                              decltype(::stellarxdr::TrustLineEntry::highAccount),
                              &::stellarxdr::TrustLineEntry::highAccount>,
                    field_ptr<::stellarxdr::TrustLineEntry,
                              decltype(::stellarxdr::TrustLineEntry::currency),
                              &::stellarxdr::TrustLineEntry::currency>,
                    field_ptr<::stellarxdr::TrustLineEntry,
                              decltype(::stellarxdr::TrustLineEntry::lowLimit),
                              &::stellarxdr::TrustLineEntry::lowLimit>,
                    field_ptr<::stellarxdr::TrustLineEntry,
                              decltype(::stellarxdr::TrustLineEntry::highLimit),
                              &::stellarxdr::TrustLineEntry::highLimit>,
                    field_ptr<::stellarxdr::TrustLineEntry,
                              decltype(::stellarxdr::TrustLineEntry::balance),
                              &::stellarxdr::TrustLineEntry::balance>,
                    field_ptr<::stellarxdr::TrustLineEntry,
                              decltype(::stellarxdr::TrustLineEntry::lowAuthSet),
                              &::stellarxdr::TrustLineEntry::lowAuthSet>,
                    field_ptr<::stellarxdr::TrustLineEntry,
                              decltype(::stellarxdr::TrustLineEntry::highAuthSet),
                              &::stellarxdr::TrustLineEntry::highAuthSet>> {
  template<typename Archive> static void
  save(Archive &ar, const ::stellarxdr::TrustLineEntry &obj) {
    archive(ar, obj.lowAccount, "lowAccount");
    archive(ar, obj.highAccount, "highAccount");
    archive(ar, obj.currency, "currency");
    archive(ar, obj.lowLimit, "lowLimit");
    archive(ar, obj.highLimit, "highLimit");
    archive(ar, obj.balance, "balance");
    archive(ar, obj.lowAuthSet, "lowAuthSet");
    archive(ar, obj.highAuthSet, "highAuthSet");
  }
  template<typename Archive> static void
  load(Archive &ar, ::stellarxdr::TrustLineEntry &obj) {
    archive(ar, obj.lowAccount, "lowAccount");
    archive(ar, obj.highAccount, "highAccount");
    archive(ar, obj.currency, "currency");
    archive(ar, obj.lowLimit, "lowLimit");
    archive(ar, obj.highLimit, "highLimit");
    archive(ar, obj.balance, "balance");
    archive(ar, obj.lowAuthSet, "lowAuthSet");
    archive(ar, obj.highAuthSet, "highAuthSet");
  }
};
} namespace stellarxdr {

struct OfferEntry {
  uint160 accountID{};
  uint32 sequence{};
  bool passive{};
  uint32 expiration{};
  
  OfferEntry() = default;
  template<typename _accountID_T,
           typename _sequence_T,
           typename _passive_T,
           typename _expiration_T>
  explicit OfferEntry(_accountID_T &&_accountID,
                      _sequence_T &&_sequence,
                      _passive_T &&_passive,
                      _expiration_T &&_expiration)
    : accountID(std::forward<_accountID_T>(_accountID)),
      sequence(std::forward<_sequence_T>(_sequence)),
      passive(std::forward<_passive_T>(_passive)),
      expiration(std::forward<_expiration_T>(_expiration)) {}
};
} namespace xdr {
template<> struct xdr_traits<::stellarxdr::OfferEntry>
  : xdr_struct_base<field_ptr<::stellarxdr::OfferEntry,
                              decltype(::stellarxdr::OfferEntry::accountID),
                              &::stellarxdr::OfferEntry::accountID>,
                    field_ptr<::stellarxdr::OfferEntry,
                              decltype(::stellarxdr::OfferEntry::sequence),
                              &::stellarxdr::OfferEntry::sequence>,
                    field_ptr<::stellarxdr::OfferEntry,
                              decltype(::stellarxdr::OfferEntry::passive),
                              &::stellarxdr::OfferEntry::passive>,
                    field_ptr<::stellarxdr::OfferEntry,
                              decltype(::stellarxdr::OfferEntry::expiration),
                              &::stellarxdr::OfferEntry::expiration>> {
  template<typename Archive> static void
  save(Archive &ar, const ::stellarxdr::OfferEntry &obj) {
    archive(ar, obj.accountID, "accountID");
    archive(ar, obj.sequence, "sequence");
    archive(ar, obj.passive, "passive");
    archive(ar, obj.expiration, "expiration");
  }
  template<typename Archive> static void
  load(Archive &ar, ::stellarxdr::OfferEntry &obj) {
    archive(ar, obj.accountID, "accountID");
    archive(ar, obj.sequence, "sequence");
    archive(ar, obj.passive, "passive");
    archive(ar, obj.expiration, "expiration");
  }
};
} namespace stellarxdr {

struct LedgerEntry {
private:
  std::uint32_t type_;
  union {
    AccountEntry account_;
    TrustLineEntry trustLine_;
    OfferEntry offer_;
  };

public:
  static_assert (sizeof (LedgerTypes) <= 4, "union discriminant must be 4 bytes");

  static int _xdr_field_number(std::uint32_t which) {
    return which == ACCOUNT ? 1
      : which == TRUSTLINE ? 2
      : which == OFFER ? 3
      : -1;
  }
  template<typename _F, typename...A> static bool
  _xdr_with_mem_ptr(_F &_f, std::uint32_t which, A&&...a) {
    switch (which) {
    case ACCOUNT:
      _f(&LedgerEntry::account_, std::forward<A>(a)...);
      return true;
    case TRUSTLINE:
      _f(&LedgerEntry::trustLine_, std::forward<A>(a)...);
      return true;
    case OFFER:
      _f(&LedgerEntry::offer_, std::forward<A>(a)...);
      return true;
    }
    return false;
  }

  std::uint32_t _xdr_discriminant() const { return type_; }
  void _xdr_discriminant(std::uint32_t which, bool validate = true) {
    int fnum = _xdr_field_number(which);
    if (fnum < 0 && validate)
      throw xdr::xdr_bad_discriminant("bad value of type in LedgerEntry");
    if (fnum != _xdr_field_number(type_)) {
      this->~LedgerEntry();
      type_ = which;
      _xdr_with_mem_ptr(xdr::field_constructor, type_, *this);
    }
  }
  LedgerEntry(LedgerTypes which = LedgerTypes{}) : type_(which) {
    _xdr_with_mem_ptr(xdr::field_constructor, type_, *this);
  }
  LedgerEntry(const LedgerEntry &source) : type_(source.type_) {
    _xdr_with_mem_ptr(xdr::field_constructor, type_, *this, source);
  }
  LedgerEntry(LedgerEntry &&source) : type_(source.type_) {
    _xdr_with_mem_ptr(xdr::field_constructor, type_, *this,
                      std::move(source));
  }
  ~LedgerEntry() { _xdr_with_mem_ptr(xdr::field_destructor, type_, *this); }
  LedgerEntry &operator=(const LedgerEntry &source) {
    if (_xdr_field_number(type_) 
        == _xdr_field_number(source.type_))
      _xdr_with_mem_ptr(xdr::field_assigner, type_, *this, source);
    else {
      this->~LedgerEntry();
      type_ = std::uint32_t(-1);
      _xdr_with_mem_ptr(xdr::field_constructor, type_, *this, source);
    }
    type_ = source.type_;
    return *this;
  }
  LedgerEntry &operator=(LedgerEntry &&source) {
    if (_xdr_field_number(type_)
         == _xdr_field_number(source.type_))
      _xdr_with_mem_ptr(xdr::field_assigner, type_, *this,
                        std::move(source));
    else {
      this->~LedgerEntry();
      type_ = std::uint32_t(-1);
      _xdr_with_mem_ptr(xdr::field_constructor, type_, *this,
                        std::move(source));
    }
    type_ = source.type_;
    return *this;
  }

  LedgerTypes type() const { return LedgerTypes(type_); }
  LedgerEntry &type(LedgerTypes _xdr_d, bool _xdr_validate = true) {
    _xdr_discriminant(_xdr_d, _xdr_validate);
    return *this;
  }

  AccountEntry &account() {
    if (_xdr_field_number(type_) == 1)
      return account_;
    throw xdr::xdr_wrong_union("LedgerEntry: account accessed when not selected");
  }
  const AccountEntry &account() const {
    if (_xdr_field_number(type_) == 1)
      return account_;
    throw xdr::xdr_wrong_union("LedgerEntry: account accessed when not selected");
  }
  TrustLineEntry &trustLine() {
    if (_xdr_field_number(type_) == 2)
      return trustLine_;
    throw xdr::xdr_wrong_union("LedgerEntry: trustLine accessed when not selected");
  }
  const TrustLineEntry &trustLine() const {
    if (_xdr_field_number(type_) == 2)
      return trustLine_;
    throw xdr::xdr_wrong_union("LedgerEntry: trustLine accessed when not selected");
  }
  OfferEntry &offer() {
    if (_xdr_field_number(type_) == 3)
      return offer_;
    throw xdr::xdr_wrong_union("LedgerEntry: offer accessed when not selected");
  }
  const OfferEntry &offer() const {
    if (_xdr_field_number(type_) == 3)
      return offer_;
    throw xdr::xdr_wrong_union("LedgerEntry: offer accessed when not selected");
  }
};
} namespace xdr {
template<> struct xdr_traits<::stellarxdr::LedgerEntry> : xdr_traits_base {
  static const bool is_class = true;
  static const bool is_union = true;
  static const bool has_fixed_size = false;

  using union_type = ::stellarxdr::LedgerEntry;
  using discriminant_type = decltype(std::declval<union_type>().type());

  static const char *union_field_name(std::uint32_t which) {
    switch (union_type::_xdr_field_number(which)) {
    case 1:
      return "AccountEntry";
    case 2:
      return "TrustLineEntry";
    case 3:
      return "OfferEntry";
    }
    return nullptr;
  }
  static const char *union_field_name(const union_type &u) {
    return union_field_name(u._xdr_discriminant());
  }

  static std::size_t serial_size(const ::stellarxdr::LedgerEntry &obj) {
    std::size_t size = 0;
    if (!obj._xdr_with_mem_ptr(field_size, obj._xdr_discriminant(), obj, size))
      throw xdr_bad_discriminant("bad value of type in LedgerEntry");
    return size + 4;
  }
  template<typename Archive> static void
  save(Archive &ar, const ::stellarxdr::LedgerEntry &obj) {
    xdr::archive(ar, obj.type(), "type");
    if (!obj._xdr_with_mem_ptr(field_archiver, obj.type(), ar, obj,
                               union_field_name(obj)))
      throw xdr_bad_discriminant("bad value of type in LedgerEntry");
  }
  template<typename Archive> static void
  load(Archive &ar, ::stellarxdr::LedgerEntry &obj) {
    discriminant_type which;
    xdr::archive(ar, which, "type");
    obj.type(which);
    obj._xdr_with_mem_ptr(field_archiver, obj.type(), ar, obj,
                          union_field_name(which));
  }
};
} namespace stellarxdr {

struct QuorumSet {
  uint32 threshold{};
  xdr::xvector<uint256> validators{};
  
  QuorumSet() = default;
  template<typename _threshold_T,
           typename _validators_T>
  explicit QuorumSet(_threshold_T &&_threshold,
                     _validators_T &&_validators)
    : threshold(std::forward<_threshold_T>(_threshold)),
      validators(std::forward<_validators_T>(_validators)) {}
};
} namespace xdr {
template<> struct xdr_traits<::stellarxdr::QuorumSet>
  : xdr_struct_base<field_ptr<::stellarxdr::QuorumSet,
                              decltype(::stellarxdr::QuorumSet::threshold),
                              &::stellarxdr::QuorumSet::threshold>,
                    field_ptr<::stellarxdr::QuorumSet,
                              decltype(::stellarxdr::QuorumSet::validators),
                              &::stellarxdr::QuorumSet::validators>> {
  template<typename Archive> static void
  save(Archive &ar, const ::stellarxdr::QuorumSet &obj) {
    archive(ar, obj.threshold, "threshold");
    archive(ar, obj.validators, "validators");
  }
  template<typename Archive> static void
  load(Archive &ar, ::stellarxdr::QuorumSet &obj) {
    archive(ar, obj.threshold, "threshold");
    archive(ar, obj.validators, "validators");
  }
};
} namespace stellarxdr {

struct Peer {
  uint32 ip{};
  uint32 port{};
  
  Peer() = default;
  template<typename _ip_T,
           typename _port_T>
  explicit Peer(_ip_T &&_ip,
                _port_T &&_port)
    : ip(std::forward<_ip_T>(_ip)),
      port(std::forward<_port_T>(_port)) {}
};
} namespace xdr {
template<> struct xdr_traits<::stellarxdr::Peer>
  : xdr_struct_base<field_ptr<::stellarxdr::Peer,
                              decltype(::stellarxdr::Peer::ip),
                              &::stellarxdr::Peer::ip>,
                    field_ptr<::stellarxdr::Peer,
                              decltype(::stellarxdr::Peer::port),
                              &::stellarxdr::Peer::port>> {
  template<typename Archive> static void
  save(Archive &ar, const ::stellarxdr::Peer &obj) {
    archive(ar, obj.ip, "ip");
    archive(ar, obj.port, "port");
  }
  template<typename Archive> static void
  load(Archive &ar, ::stellarxdr::Peer &obj) {
    archive(ar, obj.ip, "ip");
    archive(ar, obj.port, "port");
  }
};
} namespace stellarxdr {

enum MessageType : std::uint32_t {
  ERROR_MSG,
  HELLO,
  DONT_HAVE,
  GET_PEERS,
  PEERS,
  GET_HISTORY,
  HISTORY,
  GET_DELTA,
  DELTA,
  GET_TX_SET,
  TX_SET,
  GET_VALIDATIONS,
  VALIDATIONS,
  TRANSACTION,
  JSON_TRANSACTION,
  GET_QUORUMSET,
  QUORUMSET,
  FBA_MESSAGE,
};
} namespace xdr {
template<> struct xdr_traits<::stellarxdr::MessageType>
  : xdr_integral_base<::stellarxdr::MessageType, std::uint32_t> {
  static const bool is_enum = true;
  static const bool is_numeric = false;
  static const char *enum_name(::stellarxdr::MessageType val) {
    switch (val) {
    case ::stellarxdr::ERROR_MSG:
      return "ERROR_MSG";
    case ::stellarxdr::HELLO:
      return "HELLO";
    case ::stellarxdr::DONT_HAVE:
      return "DONT_HAVE";
    case ::stellarxdr::GET_PEERS:
      return "GET_PEERS";
    case ::stellarxdr::PEERS:
      return "PEERS";
    case ::stellarxdr::GET_HISTORY:
      return "GET_HISTORY";
    case ::stellarxdr::HISTORY:
      return "HISTORY";
    case ::stellarxdr::GET_DELTA:
      return "GET_DELTA";
    case ::stellarxdr::DELTA:
      return "DELTA";
    case ::stellarxdr::GET_TX_SET:
      return "GET_TX_SET";
    case ::stellarxdr::TX_SET:
      return "TX_SET";
    case ::stellarxdr::GET_VALIDATIONS:
      return "GET_VALIDATIONS";
    case ::stellarxdr::VALIDATIONS:
      return "VALIDATIONS";
    case ::stellarxdr::TRANSACTION:
      return "TRANSACTION";
    case ::stellarxdr::JSON_TRANSACTION:
      return "JSON_TRANSACTION";
    case ::stellarxdr::GET_QUORUMSET:
      return "GET_QUORUMSET";
    case ::stellarxdr::QUORUMSET:
      return "QUORUMSET";
    case ::stellarxdr::FBA_MESSAGE:
      return "FBA_MESSAGE";
    default:
      return nullptr;
    }
  }
  static const std::vector<uint32_t> &enum_values() {
    static const std::vector<uint32_t> _xdr_enum_vec = {
      ::stellarxdr::ERROR_MSG,
      ::stellarxdr::HELLO,
      ::stellarxdr::DONT_HAVE,
      ::stellarxdr::GET_PEERS,
      ::stellarxdr::PEERS,
      ::stellarxdr::GET_HISTORY,
      ::stellarxdr::HISTORY,
      ::stellarxdr::GET_DELTA,
      ::stellarxdr::DELTA,
      ::stellarxdr::GET_TX_SET,
      ::stellarxdr::TX_SET,
      ::stellarxdr::GET_VALIDATIONS,
      ::stellarxdr::VALIDATIONS,
      ::stellarxdr::TRANSACTION,
      ::stellarxdr::JSON_TRANSACTION,
      ::stellarxdr::GET_QUORUMSET,
      ::stellarxdr::QUORUMSET,
      ::stellarxdr::FBA_MESSAGE
    };
    return _xdr_enum_vec;
  }
};
} namespace stellarxdr {

struct DontHave {
  MessageType type{};
  uint256 reqHash{};
  
  DontHave() = default;
  template<typename _type_T,
           typename _reqHash_T>
  explicit DontHave(_type_T &&_type,
                    _reqHash_T &&_reqHash)
    : type(std::forward<_type_T>(_type)),
      reqHash(std::forward<_reqHash_T>(_reqHash)) {}
};
} namespace xdr {
template<> struct xdr_traits<::stellarxdr::DontHave>
  : xdr_struct_base<field_ptr<::stellarxdr::DontHave,
                              decltype(::stellarxdr::DontHave::type),
                              &::stellarxdr::DontHave::type>,
                    field_ptr<::stellarxdr::DontHave,
                              decltype(::stellarxdr::DontHave::reqHash),
                              &::stellarxdr::DontHave::reqHash>> {
  template<typename Archive> static void
  save(Archive &ar, const ::stellarxdr::DontHave &obj) {
    archive(ar, obj.type, "type");
    archive(ar, obj.reqHash, "reqHash");
  }
  template<typename Archive> static void
  load(Archive &ar, ::stellarxdr::DontHave &obj) {
    archive(ar, obj.type, "type");
    archive(ar, obj.reqHash, "reqHash");
  }
};
} namespace stellarxdr {

struct GetDelta {
  uint256 a{};
  uint256 b{};
  
  GetDelta() = default;
  template<typename _a_T,
           typename _b_T>
  explicit GetDelta(_a_T &&_a,
                    _b_T &&_b)
    : a(std::forward<_a_T>(_a)),
      b(std::forward<_b_T>(_b)) {}
};
} namespace xdr {
template<> struct xdr_traits<::stellarxdr::GetDelta>
  : xdr_struct_base<field_ptr<::stellarxdr::GetDelta,
                              decltype(::stellarxdr::GetDelta::a),
                              &::stellarxdr::GetDelta::a>,
                    field_ptr<::stellarxdr::GetDelta,
                              decltype(::stellarxdr::GetDelta::b),
                              &::stellarxdr::GetDelta::b>> {
  template<typename Archive> static void
  save(Archive &ar, const ::stellarxdr::GetDelta &obj) {
    archive(ar, obj.a, "a");
    archive(ar, obj.b, "b");
  }
  template<typename Archive> static void
  load(Archive &ar, ::stellarxdr::GetDelta &obj) {
    archive(ar, obj.a, "a");
    archive(ar, obj.b, "b");
  }
};
} namespace stellarxdr {

struct GetHistory {
  uint256 a{};
  uint256 b{};
  
  GetHistory() = default;
  template<typename _a_T,
           typename _b_T>
  explicit GetHistory(_a_T &&_a,
                      _b_T &&_b)
    : a(std::forward<_a_T>(_a)),
      b(std::forward<_b_T>(_b)) {}
};
} namespace xdr {
template<> struct xdr_traits<::stellarxdr::GetHistory>
  : xdr_struct_base<field_ptr<::stellarxdr::GetHistory,
                              decltype(::stellarxdr::GetHistory::a),
                              &::stellarxdr::GetHistory::a>,
                    field_ptr<::stellarxdr::GetHistory,
                              decltype(::stellarxdr::GetHistory::b),
                              &::stellarxdr::GetHistory::b>> {
  template<typename Archive> static void
  save(Archive &ar, const ::stellarxdr::GetHistory &obj) {
    archive(ar, obj.a, "a");
    archive(ar, obj.b, "b");
  }
  template<typename Archive> static void
  load(Archive &ar, ::stellarxdr::GetHistory &obj) {
    archive(ar, obj.a, "a");
    archive(ar, obj.b, "b");
  }
};
} namespace stellarxdr {

struct StellarMessage {
private:
  std::uint32_t type_;
  union {
    Error error_;
    Hello hello_;
    DontHave dontHave_;
    xdr::xvector<Peer> peers_;
    GetHistory historyReq_;
    History history_;
    GetDelta deltaReq_;
    xdr::xvector<LedgerEntry> deltaEntries_;
    uint256 txSetHash_;
    TransactionSet txSet_;
    uint256 ledgerHash_;
    xdr::xvector<FBAEnvelope> validations_;
    TransactionEnvelope transaction_;
    uint256 qSetHash_;
    QuorumSet quorumSet_;
    FBAEnvelope fbaMessage_;
  };

public:
  static_assert (sizeof (MessageType) <= 4, "union discriminant must be 4 bytes");

  static int _xdr_field_number(std::uint32_t which) {
    return which == ERROR_MSG ? 1
      : which == HELLO ? 2
      : which == DONT_HAVE ? 3
      : which == GET_PEERS ? 0
      : which == PEERS ? 4
      : which == GET_HISTORY ? 5
      : which == HISTORY ? 6
      : which == GET_DELTA ? 7
      : which == DELTA ? 8
      : which == GET_TX_SET ? 9
      : which == TX_SET ? 10
      : which == GET_VALIDATIONS ? 11
      : which == VALIDATIONS ? 12
      : which == TRANSACTION ? 13
      : which == GET_QUORUMSET ? 14
      : which == QUORUMSET ? 15
      : which == FBA_MESSAGE ? 16
      : -1;
  }
  template<typename _F, typename...A> static bool
  _xdr_with_mem_ptr(_F &_f, std::uint32_t which, A&&...a) {
    switch (which) {
    case ERROR_MSG:
      _f(&StellarMessage::error_, std::forward<A>(a)...);
      return true;
    case HELLO:
      _f(&StellarMessage::hello_, std::forward<A>(a)...);
      return true;
    case DONT_HAVE:
      _f(&StellarMessage::dontHave_, std::forward<A>(a)...);
      return true;
    case GET_PEERS:
      return true;
    case PEERS:
      _f(&StellarMessage::peers_, std::forward<A>(a)...);
      return true;
    case GET_HISTORY:
      _f(&StellarMessage::historyReq_, std::forward<A>(a)...);
      return true;
    case HISTORY:
      _f(&StellarMessage::history_, std::forward<A>(a)...);
      return true;
    case GET_DELTA:
      _f(&StellarMessage::deltaReq_, std::forward<A>(a)...);
      return true;
    case DELTA:
      _f(&StellarMessage::deltaEntries_, std::forward<A>(a)...);
      return true;
    case GET_TX_SET:
      _f(&StellarMessage::txSetHash_, std::forward<A>(a)...);
      return true;
    case TX_SET:
      _f(&StellarMessage::txSet_, std::forward<A>(a)...);
      return true;
    case GET_VALIDATIONS:
      _f(&StellarMessage::ledgerHash_, std::forward<A>(a)...);
      return true;
    case VALIDATIONS:
      _f(&StellarMessage::validations_, std::forward<A>(a)...);
      return true;
    case TRANSACTION:
      _f(&StellarMessage::transaction_, std::forward<A>(a)...);
      return true;
    case GET_QUORUMSET:
      _f(&StellarMessage::qSetHash_, std::forward<A>(a)...);
      return true;
    case QUORUMSET:
      _f(&StellarMessage::quorumSet_, std::forward<A>(a)...);
      return true;
    case FBA_MESSAGE:
      _f(&StellarMessage::fbaMessage_, std::forward<A>(a)...);
      return true;
    }
    return false;
  }

  std::uint32_t _xdr_discriminant() const { return type_; }
  void _xdr_discriminant(std::uint32_t which, bool validate = true) {
    int fnum = _xdr_field_number(which);
    if (fnum < 0 && validate)
      throw xdr::xdr_bad_discriminant("bad value of type in StellarMessage");
    if (fnum != _xdr_field_number(type_)) {
      this->~StellarMessage();
      type_ = which;
      _xdr_with_mem_ptr(xdr::field_constructor, type_, *this);
    }
  }
  StellarMessage(MessageType which = MessageType{}) : type_(which) {
    _xdr_with_mem_ptr(xdr::field_constructor, type_, *this);
  }
  StellarMessage(const StellarMessage &source) : type_(source.type_) {
    _xdr_with_mem_ptr(xdr::field_constructor, type_, *this, source);
  }
  StellarMessage(StellarMessage &&source) : type_(source.type_) {
    _xdr_with_mem_ptr(xdr::field_constructor, type_, *this,
                      std::move(source));
  }
  ~StellarMessage() { _xdr_with_mem_ptr(xdr::field_destructor, type_, *this); }
  StellarMessage &operator=(const StellarMessage &source) {
    if (_xdr_field_number(type_) 
        == _xdr_field_number(source.type_))
      _xdr_with_mem_ptr(xdr::field_assigner, type_, *this, source);
    else {
      this->~StellarMessage();
      type_ = std::uint32_t(-1);
      _xdr_with_mem_ptr(xdr::field_constructor, type_, *this, source);
    }
    type_ = source.type_;
    return *this;
  }
  StellarMessage &operator=(StellarMessage &&source) {
    if (_xdr_field_number(type_)
         == _xdr_field_number(source.type_))
      _xdr_with_mem_ptr(xdr::field_assigner, type_, *this,
                        std::move(source));
    else {
      this->~StellarMessage();
      type_ = std::uint32_t(-1);
      _xdr_with_mem_ptr(xdr::field_constructor, type_, *this,
                        std::move(source));
    }
    type_ = source.type_;
    return *this;
  }

  MessageType type() const { return MessageType(type_); }
  StellarMessage &type(MessageType _xdr_d, bool _xdr_validate = true) {
    _xdr_discriminant(_xdr_d, _xdr_validate);
    return *this;
  }

  Error &error() {
    if (_xdr_field_number(type_) == 1)
      return error_;
    throw xdr::xdr_wrong_union("StellarMessage: error accessed when not selected");
  }
  const Error &error() const {
    if (_xdr_field_number(type_) == 1)
      return error_;
    throw xdr::xdr_wrong_union("StellarMessage: error accessed when not selected");
  }
  Hello &hello() {
    if (_xdr_field_number(type_) == 2)
      return hello_;
    throw xdr::xdr_wrong_union("StellarMessage: hello accessed when not selected");
  }
  const Hello &hello() const {
    if (_xdr_field_number(type_) == 2)
      return hello_;
    throw xdr::xdr_wrong_union("StellarMessage: hello accessed when not selected");
  }
  DontHave &dontHave() {
    if (_xdr_field_number(type_) == 3)
      return dontHave_;
    throw xdr::xdr_wrong_union("StellarMessage: dontHave accessed when not selected");
  }
  const DontHave &dontHave() const {
    if (_xdr_field_number(type_) == 3)
      return dontHave_;
    throw xdr::xdr_wrong_union("StellarMessage: dontHave accessed when not selected");
  }
  xdr::xvector<Peer> &peers() {
    if (_xdr_field_number(type_) == 4)
      return peers_;
    throw xdr::xdr_wrong_union("StellarMessage: peers accessed when not selected");
  }
  const xdr::xvector<Peer> &peers() const {
    if (_xdr_field_number(type_) == 4)
      return peers_;
    throw xdr::xdr_wrong_union("StellarMessage: peers accessed when not selected");
  }
  GetHistory &historyReq() {
    if (_xdr_field_number(type_) == 5)
      return historyReq_;
    throw xdr::xdr_wrong_union("StellarMessage: historyReq accessed when not selected");
  }
  const GetHistory &historyReq() const {
    if (_xdr_field_number(type_) == 5)
      return historyReq_;
    throw xdr::xdr_wrong_union("StellarMessage: historyReq accessed when not selected");
  }
  History &history() {
    if (_xdr_field_number(type_) == 6)
      return history_;
    throw xdr::xdr_wrong_union("StellarMessage: history accessed when not selected");
  }
  const History &history() const {
    if (_xdr_field_number(type_) == 6)
      return history_;
    throw xdr::xdr_wrong_union("StellarMessage: history accessed when not selected");
  }
  GetDelta &deltaReq() {
    if (_xdr_field_number(type_) == 7)
      return deltaReq_;
    throw xdr::xdr_wrong_union("StellarMessage: deltaReq accessed when not selected");
  }
  const GetDelta &deltaReq() const {
    if (_xdr_field_number(type_) == 7)
      return deltaReq_;
    throw xdr::xdr_wrong_union("StellarMessage: deltaReq accessed when not selected");
  }
  xdr::xvector<LedgerEntry> &deltaEntries() {
    if (_xdr_field_number(type_) == 8)
      return deltaEntries_;
    throw xdr::xdr_wrong_union("StellarMessage: deltaEntries accessed when not selected");
  }
  const xdr::xvector<LedgerEntry> &deltaEntries() const {
    if (_xdr_field_number(type_) == 8)
      return deltaEntries_;
    throw xdr::xdr_wrong_union("StellarMessage: deltaEntries accessed when not selected");
  }
  uint256 &txSetHash() {
    if (_xdr_field_number(type_) == 9)
      return txSetHash_;
    throw xdr::xdr_wrong_union("StellarMessage: txSetHash accessed when not selected");
  }
  const uint256 &txSetHash() const {
    if (_xdr_field_number(type_) == 9)
      return txSetHash_;
    throw xdr::xdr_wrong_union("StellarMessage: txSetHash accessed when not selected");
  }
  TransactionSet &txSet() {
    if (_xdr_field_number(type_) == 10)
      return txSet_;
    throw xdr::xdr_wrong_union("StellarMessage: txSet accessed when not selected");
  }
  const TransactionSet &txSet() const {
    if (_xdr_field_number(type_) == 10)
      return txSet_;
    throw xdr::xdr_wrong_union("StellarMessage: txSet accessed when not selected");
  }
  uint256 &ledgerHash() {
    if (_xdr_field_number(type_) == 11)
      return ledgerHash_;
    throw xdr::xdr_wrong_union("StellarMessage: ledgerHash accessed when not selected");
  }
  const uint256 &ledgerHash() const {
    if (_xdr_field_number(type_) == 11)
      return ledgerHash_;
    throw xdr::xdr_wrong_union("StellarMessage: ledgerHash accessed when not selected");
  }
  xdr::xvector<FBAEnvelope> &validations() {
    if (_xdr_field_number(type_) == 12)
      return validations_;
    throw xdr::xdr_wrong_union("StellarMessage: validations accessed when not selected");
  }
  const xdr::xvector<FBAEnvelope> &validations() const {
    if (_xdr_field_number(type_) == 12)
      return validations_;
    throw xdr::xdr_wrong_union("StellarMessage: validations accessed when not selected");
  }
  TransactionEnvelope &transaction() {
    if (_xdr_field_number(type_) == 13)
      return transaction_;
    throw xdr::xdr_wrong_union("StellarMessage: transaction accessed when not selected");
  }
  const TransactionEnvelope &transaction() const {
    if (_xdr_field_number(type_) == 13)
      return transaction_;
    throw xdr::xdr_wrong_union("StellarMessage: transaction accessed when not selected");
  }
  uint256 &qSetHash() {
    if (_xdr_field_number(type_) == 14)
      return qSetHash_;
    throw xdr::xdr_wrong_union("StellarMessage: qSetHash accessed when not selected");
  }
  const uint256 &qSetHash() const {
    if (_xdr_field_number(type_) == 14)
      return qSetHash_;
    throw xdr::xdr_wrong_union("StellarMessage: qSetHash accessed when not selected");
  }
  QuorumSet &quorumSet() {
    if (_xdr_field_number(type_) == 15)
      return quorumSet_;
    throw xdr::xdr_wrong_union("StellarMessage: quorumSet accessed when not selected");
  }
  const QuorumSet &quorumSet() const {
    if (_xdr_field_number(type_) == 15)
      return quorumSet_;
    throw xdr::xdr_wrong_union("StellarMessage: quorumSet accessed when not selected");
  }
  FBAEnvelope &fbaMessage() {
    if (_xdr_field_number(type_) == 16)
      return fbaMessage_;
    throw xdr::xdr_wrong_union("StellarMessage: fbaMessage accessed when not selected");
  }
  const FBAEnvelope &fbaMessage() const {
    if (_xdr_field_number(type_) == 16)
      return fbaMessage_;
    throw xdr::xdr_wrong_union("StellarMessage: fbaMessage accessed when not selected");
  }
};
} namespace xdr {
template<> struct xdr_traits<::stellarxdr::StellarMessage> : xdr_traits_base {
  static const bool is_class = true;
  static const bool is_union = true;
  static const bool has_fixed_size = false;

  using union_type = ::stellarxdr::StellarMessage;
  using discriminant_type = decltype(std::declval<union_type>().type());

  static const char *union_field_name(std::uint32_t which) {
    switch (union_type::_xdr_field_number(which)) {
    case 1:
      return "Error";
    case 2:
      return "Hello";
    case 3:
      return "DontHave";
    case 4:
      return "Peer";
    case 5:
      return "GetHistory";
    case 6:
      return "History";
    case 7:
      return "GetDelta";
    case 8:
      return "LedgerEntry";
    case 9:
      return "uint256";
    case 10:
      return "TransactionSet";
    case 11:
      return "uint256";
    case 12:
      return "FBAEnvelope";
    case 13:
      return "TransactionEnvelope";
    case 14:
      return "uint256";
    case 15:
      return "QuorumSet";
    case 16:
      return "FBAEnvelope";
    }
    return nullptr;
  }
  static const char *union_field_name(const union_type &u) {
    return union_field_name(u._xdr_discriminant());
  }

  static std::size_t serial_size(const ::stellarxdr::StellarMessage &obj) {
    std::size_t size = 0;
    if (!obj._xdr_with_mem_ptr(field_size, obj._xdr_discriminant(), obj, size))
      throw xdr_bad_discriminant("bad value of type in StellarMessage");
    return size + 4;
  }
  template<typename Archive> static void
  save(Archive &ar, const ::stellarxdr::StellarMessage &obj) {
    xdr::archive(ar, obj.type(), "type");
    if (!obj._xdr_with_mem_ptr(field_archiver, obj.type(), ar, obj,
                               union_field_name(obj)))
      throw xdr_bad_discriminant("bad value of type in StellarMessage");
  }
  template<typename Archive> static void
  load(Archive &ar, ::stellarxdr::StellarMessage &obj) {
    discriminant_type which;
    xdr::archive(ar, which, "type");
    obj.type(which);
    obj._xdr_with_mem_ptr(field_archiver, obj.type(), ar, obj,
                          union_field_name(which));
  }
};
} namespace stellarxdr {

}

#endif // !__XDR_PROTOCOL_HH_INCLUDED__
