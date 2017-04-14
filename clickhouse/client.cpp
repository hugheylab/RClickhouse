#include "client.h"
#include "protocol.h"
#include "wire_format.h"

#include "base/coded.h"
#include "base/socket.h"
#include "columns/factory.h"

#include <atomic>
#include <system_error>
#include <vector>
#include <iostream>

#define DBMS_NAME                                       "ClickHouse"
#define DBMS_VERSION_MAJOR                              1
#define DBMS_VERSION_MINOR                              1
#define REVISION                                        54126

#define DBMS_MIN_REVISION_WITH_TEMPORARY_TABLES         50264
#define DBMS_MIN_REVISION_WITH_TOTAL_ROWS_IN_PROGRESS   51554
#define DBMS_MIN_REVISION_WITH_BLOCK_INFO               51903
#define DBMS_MIN_REVISION_WITH_CLIENT_INFO              54032
#define DBMS_MIN_REVISION_WITH_SERVER_TIMEZONE          54058
#define DBMS_MIN_REVISION_WITH_QUOTA_KEY_IN_CLIENT_INFO 54060

namespace clickhouse {

struct ClientInfo {
    uint8_t iface_type = 1; // TCP
    uint8_t query_kind;
    std::string initial_user;
    std::string initial_query_id;
    std::string quota_key;
    std::string os_user;
    std::string client_hostname;
    std::string client_name;
    std::string initial_address = "[::ffff:127.0.0.1]:0";
    uint64_t client_version_major = 0;
    uint64_t client_version_minor = 0;
    uint32_t client_revision = 0;
};

struct ServerInfo {
    std::string name;
    std::string timezone;
    uint64_t    version_major;
    uint64_t    version_minor;
    uint64_t    revision;
};


class Client::Impl {
public:
     Impl(const ClientOptions& opts);
    ~Impl();

    void ExecuteQuery(Query query);

    void Insert(const std::string& table_name, const Block& block);

    void Ping();

private:
    bool Handshake();

    bool ReceivePacket(uint64_t* server_packet = nullptr);

    void SendQuery(const std::string& query);

    void SendData(const Block& block);

    bool SendHello();

    bool ReceiveHello();

    /// Reads data packet form input stream.
    bool ReceiveData();

    /// Reads exception packet form input stream.
    bool ReceiveException(bool rethrow = false);

private:
    void Disconnect() {
        socket_.Close();
    }

private:
    class EnsureNull {
    public:
        inline EnsureNull(QueryEvents* ev, QueryEvents** ptr)
            : ptr_(ptr)
        {
            if (ptr_) {
                *ptr_ = ev;
            }
        }

        inline ~EnsureNull() {
            if (ptr_) {
                *ptr_ = nullptr;
            }
        }

    private:
        QueryEvents** ptr_;

    };


    const ClientOptions options_;
    QueryEvents* events_;
    uint64_t query_id_;

    SocketHolder socket_;

    SocketInput socket_input_;
    BufferedInput buffered_input_;
    CodedInputStream input_;

    SocketOutput socket_output_;
    BufferedOutput buffered_output_;
    CodedOutputStream output_;

    ServerInfo server_info_;
};

static uint64_t GenerateQueryId() {
    static std::atomic<uint64_t> counter;

    return ++counter;
}

Client::Impl::Impl(const ClientOptions& opts)
    : options_(opts)
    , events_(nullptr)
    , query_id_(GenerateQueryId())
    , socket_(SocketConnect(NetworkAddress(opts.host, std::to_string(opts.port))))
    , socket_input_(socket_)
    , buffered_input_(&socket_input_)
    , input_(&buffered_input_)
    , socket_output_(socket_)
    , buffered_output_(&socket_output_)
    , output_(&buffered_output_)
{
    if (socket_.Closed()) {
        throw std::system_error(errno, std::system_category());
    }
    if (!Handshake()) {
        throw std::runtime_error("fail to connect to " + options_.host);
    }
}

Client::Impl::~Impl() {
    Disconnect();
}

void Client::Impl::ExecuteQuery(Query query) {
    EnsureNull en(static_cast<QueryEvents*>(&query), &events_);

    // TODO check connection

    SendQuery(query.GetText());

    while (ReceivePacket()) {
        ;
    }
}

void Client::Impl::Insert(const std::string& table_name, const Block& block) {
    // TODO check connection

    SendQuery("INSERT INTO " + table_name + " VALUES");

    uint64_t server_packet;
    // Receive data packet.
    while (true) {
        bool ret = ReceivePacket(&server_packet);

        if (!ret) {
            std::runtime_error("fail to receive data packet");
        }
        if (server_packet == ServerCodes::Data) {
            break;
        }
        if (server_packet == ServerCodes::Progress) {
            continue;
        }
    }

    // Send data.
    SendData(block);
    // Send empty block as marker of
    // end of data.
    SendData(Block());

    // Wait for EOS.
    while (ReceivePacket()) {
        ;
    }
}

void Client::Impl::Ping() {
    WireFormat::WriteUInt64(&output_, ClientCodes::Ping);
    output_.Flush();

    ReceivePacket();
}

bool Client::Impl::Handshake() {
    if (!SendHello()) {
        return false;
    }
    if (!ReceiveHello()) {
        return false;
    }
    return true;
}

bool Client::Impl::ReceivePacket(uint64_t* server_packet) {
    uint64_t packet_type = 0;

    if (!input_.ReadVarint64(&packet_type)) {
        return false;
    }
    if (server_packet) {
        *server_packet = packet_type;
    }

    switch (packet_type) {
    case ServerCodes::Data: {
        if (!ReceiveData()) {
            std::runtime_error("can't read data packet from input stream");
        }
        return true;
    }

    case ServerCodes::Exception: {
        ReceiveException();
        return false;
    }

    case ServerCodes::ProfileInfo: {
        Profile profile;

        if (!WireFormat::ReadUInt64(&input_, &profile.rows)) {
            return false;
        }
        if (!WireFormat::ReadUInt64(&input_, &profile.blocks)) {
            return false;
        }
        if (!WireFormat::ReadUInt64(&input_, &profile.bytes)) {
            return false;
        }
        if (!WireFormat::ReadFixed(&input_, &profile.applied_limit)) {
            return false;
        }
        if (!WireFormat::ReadUInt64(&input_, &profile.rows_before_limit)) {
            return false;
        }
        if (!WireFormat::ReadFixed(&input_, &profile.calculated_rows_before_limit)) {
            return false;
        }

        if (events_) {
            events_->OnProfile(profile);
        }

        return true;
    }

    case ServerCodes::Progress: {
        Progress info;

        if (!WireFormat::ReadUInt64(&input_, &info.rows)) {
            return false;
        }
        if (!WireFormat::ReadUInt64(&input_, &info.bytes)) {
            return false;
        }
        if (REVISION >= DBMS_MIN_REVISION_WITH_TOTAL_ROWS_IN_PROGRESS) {
            if (!WireFormat::ReadUInt64(&input_, &info.total_rows)) {
                return false;
            }
        }

        if (events_) {
            events_->OnProgress(info);
        }

        return true;
    }

    case ServerCodes::Pong: {
        return true;
    }

    case ServerCodes::EndOfStream: {
        if (events_) {
            events_->OnFinish();
        }
        return false;
    }

    default:
        throw std::runtime_error("unimplemented " + std::to_string((int)packet_type));
        break;
    }

    return false;
}

bool Client::Impl::ReceiveData() {
    if (REVISION >= DBMS_MIN_REVISION_WITH_TEMPORARY_TABLES) {
        std::string table_name;

        if (!WireFormat::ReadString(&input_, &table_name)) {
            return false;
        }
    }
    // Additional information about block.
    if (REVISION >= DBMS_MIN_REVISION_WITH_BLOCK_INFO) {
        uint64_t num;
        BlockInfo info;

        // BlockInfo
        if (!WireFormat::ReadUInt64(&input_, &num)) {
            return false;
        }
        if (!WireFormat::ReadFixed(&input_, &info.is_overflows)) {
            return false;
        }
        if (!WireFormat::ReadUInt64(&input_, &num)) {
            return false;
        }
        if (!WireFormat::ReadFixed(&input_, &info.bucket_num)) {
            return false;
        }
        if (!WireFormat::ReadUInt64(&input_, &num)) {
            return false;
        }

        // TODO use data
    }

    uint64_t num_columns = 0;
    uint64_t num_rows = 0;

    if (!WireFormat::ReadUInt64(&input_, &num_columns)) {
        return false;
    }
    if (!WireFormat::ReadUInt64(&input_, &num_rows)) {
        return false;
    }

    Block block(num_columns, num_rows);

    for (size_t i = 0; i < num_columns; ++i) {
        std::string name;
        std::string type;

        if (!WireFormat::ReadString(&input_, &name)) {
            return false;
        }
        if (!WireFormat::ReadString(&input_, &type)) {
            return false;
        }

        if (ColumnRef col = CreateColumnByType(type)) {
            if (num_rows && !col->Load(&input_, num_rows)) {
                throw std::runtime_error("can't load");
            }

            block.AppendColumn(name, col);
        } else {
            throw std::runtime_error(std::string("unsupported column type: ") + type);
        }
    }

    if (events_) {
        events_->OnData(block);
    }

    return false;
}

bool Client::Impl::ReceiveException(bool rethrow) {
    std::unique_ptr<Exception> e(new Exception);
    Exception* current = e.get();

    do {
        bool has_nested = false;

        if (!WireFormat::ReadFixed(&input_, &current->code)) {
            return false;
        }
        if (!WireFormat::ReadString(&input_, &current->name)) {
            return false;
        }
        if (!WireFormat::ReadString(&input_, &current->display_text)) {
            return false;
        }
        if (!WireFormat::ReadString(&input_, &current->stack_trace)) {
            return false;
        }
        if (!WireFormat::ReadFixed(&input_, &has_nested)) {
            return false;
        }

        if (has_nested) {
            current->nested.reset(new Exception);
            current = current->nested.get();
        } else {
            break;
        }
    } while (true);

    if (events_) {
        events_->OnServerException(*e);
    }

    if (rethrow || options_.rethrow_exceptions) {
        throw ServerException(std::move(e));
    }

    return true;
}

void Client::Impl::SendQuery(const std::string& query) {
    WireFormat::WriteUInt64(&output_, ClientCodes::Query);
    WireFormat::WriteString(&output_, std::to_string(query_id_));

    /// Client info.
    if (server_info_.revision >= DBMS_MIN_REVISION_WITH_CLIENT_INFO) {
        ClientInfo info;

        info.query_kind = 1;
        info.client_name = "ClickHouse client";
        info.client_version_major = DBMS_VERSION_MAJOR;
        info.client_version_minor = DBMS_VERSION_MINOR;
        info.client_revision = REVISION;


        WireFormat::WriteFixed(&output_, info.query_kind);
        WireFormat::WriteString(&output_, info.initial_user);
        WireFormat::WriteString(&output_, info.initial_query_id);
        WireFormat::WriteString(&output_, info.initial_address);
        WireFormat::WriteFixed(&output_, info.iface_type);

        WireFormat::WriteString(&output_, info.os_user);
        WireFormat::WriteString(&output_, info.client_hostname);
        WireFormat::WriteString(&output_, info.client_name);
        WireFormat::WriteUInt64(&output_, info.client_version_major);
        WireFormat::WriteUInt64(&output_, info.client_version_minor);
        WireFormat::WriteUInt64(&output_, info.client_revision);

        if (server_info_.revision >= DBMS_MIN_REVISION_WITH_QUOTA_KEY_IN_CLIENT_INFO)
            WireFormat::WriteString(&output_, info.quota_key);
    }

    /// Per query settings.
    //if (settings)
    //    settings->serialize(*out);
    //else
    WireFormat::WriteString(&output_, std::string());

    WireFormat::WriteUInt64(&output_, Stages::Complete);
    WireFormat::WriteUInt64(&output_, CompressionState::Disable);
    WireFormat::WriteString(&output_, query);
    // Send empty block as marker of
    // end of data
    SendData(Block());

    output_.Flush();
}

void Client::Impl::SendData(const Block& block) {
    WireFormat::WriteUInt64(&output_, ClientCodes::Data);

    if (server_info_.revision >= DBMS_MIN_REVISION_WITH_TEMPORARY_TABLES) {
        WireFormat::WriteString(&output_, std::string());
    }

    /// Дополнительная информация о блоке.
    if (server_info_.revision >= DBMS_MIN_REVISION_WITH_BLOCK_INFO) {
        WireFormat::WriteUInt64(&output_, 1);
        WireFormat::WriteFixed (&output_, block.Info().is_overflows);
        WireFormat::WriteUInt64(&output_, 2);
        WireFormat::WriteFixed (&output_, block.Info().bucket_num);
        WireFormat::WriteUInt64(&output_, 0);
    }

    WireFormat::WriteUInt64(&output_, block.GetColumnCount());
    WireFormat::WriteUInt64(&output_, block.GetRowCount());

    for (Block::Iterator bi(block); bi.IsValid(); bi.Next()) {
        WireFormat::WriteString(&output_, bi.Name());
        WireFormat::WriteString(&output_, bi.Type()->GetName());

        bi.Column()->Save(&output_);
    }

    output_.Flush();
}

bool Client::Impl::SendHello() {
    WireFormat::WriteUInt64(&output_, ClientCodes::Hello);
    WireFormat::WriteString(&output_, std::string(DBMS_NAME) + " client");
    WireFormat::WriteUInt64(&output_, DBMS_VERSION_MAJOR);
    WireFormat::WriteUInt64(&output_, DBMS_VERSION_MINOR);
    WireFormat::WriteUInt64(&output_, REVISION);
    WireFormat::WriteString(&output_, options_.default_database);
    WireFormat::WriteString(&output_, options_.user);
    WireFormat::WriteString(&output_, options_.password);

    output_.Flush();

    return true;
}

bool Client::Impl::ReceiveHello() {
    uint64_t packet_type = 0;

    if (!input_.ReadVarint64(&packet_type)) {
        return false;
    }

    if (packet_type == ServerCodes::Hello) {
        if (!WireFormat::ReadString(&input_, &server_info_.name)) {
            return false;
        }
        if (!WireFormat::ReadUInt64(&input_, &server_info_.version_major)) {
            return false;
        }
        if (!WireFormat::ReadUInt64(&input_, &server_info_.version_minor)) {
            return false;
        }
        if (!WireFormat::ReadUInt64(&input_, &server_info_.revision)) {
            return false;
        }

        if (server_info_.revision >= DBMS_MIN_REVISION_WITH_SERVER_TIMEZONE) {
            if (!WireFormat::ReadString(&input_, &server_info_.timezone)) {
                return false;
            }
        }

        return true;
    } else if (packet_type == ServerCodes::Exception) {
        ReceiveException(true);
        return false;
    }

    return false;
}


Client::Client(const ClientOptions& opts)
    : options_(opts)
    , impl_(new Impl(opts))
{
}

Client::~Client()
{ }

void Client::Execute(const Query& query) {
    impl_->ExecuteQuery(query);
}

void Client::Select(const std::string& query, SelectCallback cb) {
    Execute(Query(query).OnData(cb));
}

void Client::Select(const Query& query) {
    Execute(query);
}

void Client::Insert(const std::string& table_name, const Block& block) {
    impl_->Insert(table_name, block);
}

void Client::Ping() {
    impl_->Ping();
}

}
