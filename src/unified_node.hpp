#ifndef BLANK_QUICK_UNIFIED_NODE_HPP // NOLINT(llvm-header-guard)
#define BLANK_QUICK_UNIFIED_NODE_HPP

#include <array>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

// 统一节点模型（遵循 ISO C++20，仅依赖标准库，对应 Mihomo 内核提交 3f27ef39）
// 采用四层金字塔架构与冷热分离所有权设计
namespace node_model {

// ISO C++ 聚合体保持自然对齐与显式类内初始化
// NOLINTBEGIN(altera-struct-pack-align, readability-redundant-member-init)

inline constexpr std::string_view kernel_revision{"3f27ef39f8c96bbfbc8b0b9bb10f5c9ef60149d8"};

// 第 1 层：全协议通用核心（热数据）。所有出站协议必有名称与协议类型；无端点出站使用 nullopt
enum class protocol_type : std::int8_t {
    unspecified,
    shadowsocks,
    shadowsocksr,
    http,
    socks5,
    gost_relay,
    vmess,
    vless,
    snell,
    trojan,
    hysteria,
    hysteria2,
    wireguard,
    tuic,
    shadowquic,
    direct,
    dns,
    reject,
    rematch,
    ssh,
    mieru,
    anytls,
    sudoku,
    masque,
    trusttunnel,
    openvpn,
    tailscale,
    zerotier,
    easytier,
};

// 远程目标端点（服务器地址与端口）
struct remote_endpoint {
    std::string host{};
    std::int64_t port{};
};

// 节点核心标识（名称、协议类型、可选远程端点）
struct universal_core {
    std::string name{};
    protocol_type protocol{protocol_type::unspecified};
    std::optional<remote_endpoint> remote{};
};

// 第 2 层：通用链路与传输控制（冷数据）。跨协议通用的安全层、传输层与拨号控制
using string_map = std::vector<std::pair<std::string, std::string>>;
using string_list_map = std::vector<std::pair<std::string, std::vector<std::string>>>;

// IP 版本偏好策略
enum class ip_preference : std::int8_t { dual, ipv4, ipv6, ipv4_prefer, ipv6_prefer };

// TLS 基础配置
struct tls_config {
    std::optional<bool> enabled{};
    std::string server_name{};
    std::optional<std::vector<std::string>> alpn{};
    bool skip_cert_verify{};
    std::string name_cert_verify{};
    std::string certificate_fingerprint{};
    std::string certificate{};
    std::string private_key{};
    std::string client_fingerprint{};
    bool disable_sni{};
};

// ECH（加密客户端问候）配置
struct ech_config {
    bool enable{};
    std::string config{};
    std::string query_server_name{};
};

// 跨协议共享的 REALITY 配置
struct reality_config {
    std::string public_key{};
    std::string short_id{};
    bool support_x25519mlkem768{};
};

// ShadowTLS 伪装配置
struct shadow_tls_config {
    std::string password{};
    std::int64_t version{};
};

// Restls 伪装配置
struct restls_config {
    std::string password{};
    std::string version_hint{};
    std::string restls_script{};
};

// JLS 伪装配置
struct jls_config {
    std::string username{};
    std::string password{};
};

// TLS 镜像时间规格
struct tls_mirror_time_spec {
    std::int64_t base_nanoseconds{};
    std::int64_t uniform_random_multiplier_nanoseconds{};
};

// TLS 镜像传输层填充开关
struct tls_mirror_transport_layer_padding {
    bool enabled{};
};

// TLS 镜像连接注册
struct tls_mirror_connection_enrolment {
    std::string primary_ingress_outbound{};
    std::string primary_egress_outbound{};
};

// TLS 镜像流量请求头
struct tls_mirror_traffic_header {
    std::string name{};
    std::string value{};
    std::vector<std::string> values{};
};

// TLS 镜像转移候选
struct tls_mirror_traffic_transfer_candidate {
    std::int64_t weight{};
    std::int64_t goto_location{};
};

// TLS 镜像流量步进定义
struct tls_mirror_traffic_step {
    std::string name{};
    std::string host{};
    std::string path{};
    std::string method{};
    std::vector<tls_mirror_traffic_header> headers{};
    std::vector<tls_mirror_traffic_transfer_candidate> next_step{};
    bool connection_ready{};
    bool connection_recall_exit{};
    tls_mirror_time_spec wait_time{};
    bool h2_do_not_wait_for_download_finish{};
};

// TLS 镜像流量生成器
struct tls_mirror_traffic_generator {
    std::vector<tls_mirror_traffic_step> steps{};
};

// TLSMirror 流量伪装总配置
struct tlsmirror_config {
    std::string primary_key{};
    std::vector<int> explicit_nonce_ciphersuites{};
    tls_mirror_time_spec defer_instance_derived_write_time{};
    tls_mirror_transport_layer_padding transport_layer_padding{};
    std::optional<tls_mirror_connection_enrolment> connection_enrolment{};
    tls_mirror_traffic_generator embedded_traffic_generator{};
    bool sequence_watermarking_enabled{};
};

// 安全与伪装层聚合配置
struct security_config {
    tls_config tls{};
    std::optional<ech_config> ech{};
    std::optional<reality_config> reality{};
    std::optional<shadow_tls_config> shadow_tls{};
    std::optional<restls_config> restls{};
    std::optional<jls_config> jls{};
    std::optional<tlsmirror_config> tlsmirror{};
};

// HTTP 传输层配置
struct http_transport {
    std::string method{};
    std::vector<std::string> path{};
    string_list_map headers{};
};

// HTTP/2 传输层配置
struct http2_transport {
    std::vector<std::string> host{};
    std::string path{};
};

// gRPC 传输层配置
struct grpc_transport {
    std::string grpc_service_name{};
    std::string grpc_user_agent{};
    std::int64_t ping_interval{};
    std::int64_t max_connections{};
    std::int64_t min_streams{};
    std::int64_t max_streams{};
};

// WebSocket 传输层配置
struct websocket_transport {
    std::string path{};
    string_map headers{};
    std::int64_t max_early_data{};
    std::string early_data_header_name{};
    bool v2ray_http_upgrade{};
    bool v2ray_http_upgrade_fast_open{};
};

// mKCP 传输层配置
struct mkcp_transport {
    std::int64_t mtu{};
    std::int64_t tti{};
    std::int64_t uplink_capacity{};
    std::int64_t downlink_capacity{};
    bool congestion{};
    std::int64_t write_buffer{};
    std::int64_t read_buffer{};
    std::string seed{};
    std::string header{};
};

// Mekya 传输层配置
struct mekya_transport {
    std::string url{};
    std::int64_t h2_pool_size{};
    std::int64_t max_write_delay{};
    std::int64_t max_request_size{};
    std::int64_t polling_interval_initial{};
    std::int64_t max_write_size{};
    std::int64_t max_write_duration_ms{};
    std::int64_t max_simultaneous_write_connection{};
    std::int64_t packet_writing_buffer{};
    mkcp_transport kcp{};
};

// XHTTP 连接复用配置
struct xhttp_reuse_config {
    std::string max_concurrency{};
    std::string max_connections{};
    std::string c_max_reuse_times{};
    std::string h_max_request_times{};
    std::string h_max_reusable_secs{};
    std::int64_t h_keep_alive_period{};
};

// XHTTP 下行通道分离与独立覆盖配置
struct xhttp_download_config {
    std::optional<std::string> path{};
    std::optional<std::string> host{};
    std::optional<string_map> headers{};
    std::optional<xhttp_reuse_config> reuse_settings{};
    std::optional<std::string> server{};
    std::optional<std::int64_t> port{};
    std::optional<bool> tls{};
    std::optional<std::vector<std::string>> alpn{};
    std::optional<ech_config> ech_opts{};
    std::optional<shadow_tls_config> shadow_tls_opts{};
    std::optional<restls_config> restls_opts{};
    std::optional<jls_config> jls_opts{};
    std::optional<reality_config> reality_opts{};
    std::optional<bool> skip_cert_verify{};
    std::optional<std::string> name_cert_verify{};
    std::optional<std::string> fingerprint{};
    std::optional<std::string> certificate{};
    std::optional<std::string> private_key{};
    std::optional<std::string> servername{};
    std::optional<std::string> client_fingerprint{};
};

// XHTTP 传输层配置
struct xhttp_transport {
    std::string path{};
    std::string host{};
    std::string mode{};
    string_map headers{};
    bool no_grpc_header{};
    std::string x_padding_bytes{};
    bool x_padding_obfs_mode{};
    std::string x_padding_key{};
    std::string x_padding_header{};
    std::string x_padding_placement{};
    std::string x_padding_method{};
    std::string uplink_http_method{};
    std::string session_placement{};
    std::string session_key{};
    std::string session_table{};
    std::string session_length{};
    std::string seq_placement{};
    std::string seq_key{};
    std::string uplink_data_placement{};
    std::string uplink_data_key{};
    std::string uplink_chunk_size{};
    std::string sc_max_each_post_bytes{};
    std::string sc_min_posts_interval_ms{};
    std::optional<xhttp_reuse_config> reuse_settings{};
    std::optional<xhttp_download_config> download_settings{};
};

// TCP Brutal 拥塞控制配置
struct brutal_config {
    bool enabled{};
    std::string uplink{};
    std::string downlink{};
};

// Smux 多路复用配置
struct smux_config {
    bool enabled{};
    std::string protocol{};
    std::int64_t max_connections{};
    std::int64_t min_streams{};
    std::int64_t max_streams{};
    bool padding{};
    bool statistic{};
    bool only_tcp{};
    brutal_config brutal_opts{};
};

// IP 栈模式与拥塞控制配置
struct ip_stack_config {
    std::string mode{};
    std::string congestion_controller{};
};

// 传输层载荷强类型变体
using transport_payload =
    std::variant<std::monostate, websocket_transport, grpc_transport, http_transport,
                 http2_transport, xhttp_transport, mkcp_transport, mekya_transport>;

// UDP 数据包封装与编码控制（XUDP、PacketAddr、UDP-over-TCP 等）
struct packet_control {
    bool packet_addr{};
    bool xudp{};
    std::string packet_encoding{};
    bool udp_over_tcp{};
    std::int64_t udp_over_tcp_version{};
    bool udp_over_stream{};
    std::int64_t udp_over_stream_version{};
};

// 带宽与上下行速率控制
struct bandwidth_control {
    std::string uplink{};
    std::string downlink{};
    std::int64_t up_speed{};
    std::int64_t down_speed{};
};

// 端口跳跃时间间隔（支持秒数或字符串）
using hopping_interval = std::variant<std::monostate, std::chrono::seconds, std::string>;

// 端口跳跃控制配置
struct port_hopping_control {
    std::string ports{};
    hopping_interval interval{};
};

// QUIC 传输与拥塞控制参数
struct quic_control {
    std::int64_t recv_window_conn{};
    std::int64_t recv_window{};
    bool disable_mtu_discovery{};
    bool fast_open{};
    std::int64_t cwnd{};
    std::string bbr_profile{};
    std::int64_t udp_mtu{};
    std::int64_t initial_stream_receive_window{};
    std::int64_t max_stream_receive_window{};
    std::int64_t initial_connection_receive_window{};
    std::int64_t max_connection_receive_window{};
    std::chrono::milliseconds heartbeat_interval{};
    bool reduce_rtt{};
    std::chrono::milliseconds request_timeout{};
    std::string udp_relay_mode{};
    std::string congestion_controller{};
    std::int64_t max_udp_relay_packet_size{};
    std::int64_t max_open_streams{};
    std::int64_t max_datagram_frame_size{};
    std::vector<std::string> quic_versions{};
    bool zero_rtt{};
    std::chrono::milliseconds keep_alive_interval{};
};

// 通用链路控制聚合（前置代理、网络模式、安全、传输载荷等）
struct link_control {
    std::string dialer_proxy{};
    std::string interface_name{};
    std::int64_t routing_mark{};
    ip_preference ip_policy{ip_preference::dual};
    bool tcp_fast_open{};
    bool multipath_tcp{};
    std::optional<bool> udp{};
    std::chrono::seconds handshake_timeout{};
    security_config security{};
    transport_payload transport{};
    std::optional<smux_config> smux{};
    packet_control packet{};
    bandwidth_control bandwidth{};
    port_hopping_control port_hopping{};
    quic_control quic{};
};

// 第 3 层：各协议专有凭证与参数（冷数据，强类型无类型擦除）

// Simple-obfs 插件参数
struct simple_obfs_param {
    std::string mode{};
    std::string host{"bing.com"};
};

// V2Ray-Plugin 插件参数
struct v2ray_plugin_param {
    std::string mode{};
    std::string host{"bing.com"};
    std::string path{};
    bool tls{};
    ech_config ech_opts{};
    std::string fingerprint{};
    std::string certificate{};
    std::string private_key{};
    string_map headers{};
    bool skip_cert_verify{};
    std::string name_cert_verify{};
    bool mux{true};
    bool v2ray_http_upgrade{};
    bool v2ray_http_upgrade_fast_open{};
};

// Gost-Plugin 插件参数
struct gost_plugin_param {
    std::string mode{};
    std::string host{"bing.com"};
    std::string path{};
    bool tls{};
    ech_config ech_opts{};
    std::string fingerprint{};
    std::string certificate{};
    std::string private_key{};
    string_map headers{};
    bool skip_cert_verify{};
    std::string name_cert_verify{};
    bool mux{true};
};

// ShadowTLS 插件参数
struct shadow_tls_plugin_param {
    std::string password{};
    std::string host{};
    std::string fingerprint{};
    std::string certificate{};
    std::string private_key{};
    bool skip_cert_verify{};
    std::string name_cert_verify{};
    std::int64_t version{2};
    std::optional<std::vector<std::string>> alpn{};
};

// Restls 插件参数
struct restls_plugin_param {
    std::string password{};
    std::string host{};
    std::string version_hint{};
    std::string restls_script{};
    std::string fingerprint{};
    bool skip_cert_verify{};
    std::string name_cert_verify{};
    bool force_tls12{};
};

// JLS 插件参数
struct jls_plugin_param {
    std::string host{};
    std::string username{};
    std::string password{};
    std::vector<std::string> alpn{};
};

// Kcptun 插件参数
struct kcptun_plugin_param {
    std::string key_material{};
    std::string crypt{};
    std::string mode{};
    std::int64_t conn{};
    std::int64_t autoexpire{};
    std::int64_t scavengettl{};
    std::int64_t mtu{};
    std::int64_t ratelimit{};
    std::int64_t sndwnd{};
    std::int64_t rcvwnd{};
    std::int64_t datashard{};
    std::int64_t parityshard{};
    std::int64_t dscp{};
    bool nocomp{};
    bool acknodelay{};
    std::int64_t nodelay{};
    std::int64_t interval{};
    std::int64_t resend{};
    std::int64_t network_congestion{};
    std::int64_t sockbuf{};
    std::int64_t smuxver{};
    std::int64_t smuxbuf{};
    std::int64_t framesize{};
    std::int64_t streambuf{};
    std::int64_t keepalive{};
};

// Shadowsocks 混淆插件变体
using shadowsocks_plugin = std::variant<std::monostate, simple_obfs_param, v2ray_plugin_param,
                                        gost_plugin_param, shadow_tls_plugin_param,
                                        restls_plugin_param, jls_plugin_param, kcptun_plugin_param>;
// Snell 混淆变体
using snell_obfuscation = std::variant<std::monostate, simple_obfs_param, shadow_tls_plugin_param,
                                       restls_plugin_param, jls_plugin_param>;

// Shadowsocks 专有参数
struct shadowsocks_param {
    static constexpr protocol_type protocol_tag{protocol_type::shadowsocks};

    std::string password{};
    std::string cipher{};
    shadowsocks_plugin plugin{};
};

// ShadowsocksR 专有参数
struct shadowsocksr_param {
    static constexpr protocol_type protocol_tag{protocol_type::shadowsocksr};

    std::string password{};
    std::string cipher{};
    std::string obfs{};
    std::string obfs_param{};
    std::string protocol{};
    std::string protocol_param{};
};

// Socks5 专有参数
struct socks5_param {
    static constexpr protocol_type protocol_tag{protocol_type::socks5};

    std::string username{};
    std::string password{};
};

// HTTP 代理专有参数
struct http_param {
    static constexpr protocol_type protocol_tag{protocol_type::http};

    std::string username{};
    std::string password{};
    string_map headers{};
};

// VMess 专有参数
struct vmess_param {
    static constexpr protocol_type protocol_tag{protocol_type::vmess};

    std::string uuid{};
    std::int64_t alter_id{};
    std::string cipher{};
    bool global_padding{};
    bool authenticated_length{};
};

// VLESS 专有参数
struct vless_param {
    static constexpr protocol_type protocol_tag{protocol_type::vless};

    std::string uuid{};
    std::string flow{};
    std::string encryption{};
};

// Snell 专有参数
struct snell_param {
    static constexpr protocol_type protocol_tag{protocol_type::snell};

    std::string pre_shared_key{};
    std::int64_t version{};
    bool reuse{};
    snell_obfuscation obfs_opts{};
};

// Trojan 嵌套 SS 选项
struct trojan_ss_option {
    bool enabled{};
    std::string method{};
    std::string password{};
};

// Trojan 专有参数
struct trojan_param {
    static constexpr protocol_type protocol_tag{protocol_type::trojan};

    std::string password{};
    trojan_ss_option ss_opts{};
};

// Hysteria v1 专有参数
struct hysteria_param {
    static constexpr protocol_type protocol_tag{protocol_type::hysteria};

    std::string protocol{};
    std::string obfs_protocol{};
    std::string auth{};
    std::string auth_str{};
    std::string obfs{};
};

// Hysteria 2 Realm 扩展选项
struct hysteria2_realm_option {
    bool enable{};
    std::string server_url{};
    std::string token{};
    std::string realm_id{};
    std::vector<std::string> stun_servers{};
    std::string sni{};
    bool skip_cert_verify{};
    std::string name_cert_verify{};
    std::string fingerprint{};
    std::string certificate{};
    std::string private_key{};
    std::vector<std::string> alpn{};
};

// Hysteria 2 专有参数
struct hysteria2_param {
    static constexpr protocol_type protocol_tag{protocol_type::hysteria2};

    std::string password{};
    std::string obfs{};
    std::string obfs_password{};
    std::int64_t obfs_min_packet_size{};
    std::int64_t obfs_max_packet_size{};
    hysteria2_realm_option realm_opts{};
};

// AmneziaWG (AWG) 混淆扩展参数
struct amnezia_wg_param {
    std::int64_t version{};
    std::int64_t junk_count{};
    std::int64_t jmin{};
    std::int64_t jmax{};
    std::int64_t awg_s1{};
    std::int64_t awg_s2{};
    std::int64_t awg_s3{};
    std::int64_t awg_s4{};
    std::string awg_h1{};
    std::string awg_h2{};
    std::string awg_h3{};
    std::string awg_h4{};
    std::string awg_i1{};
    std::string awg_i2{};
    std::string awg_i3{};
    std::string awg_i4{};
    std::string awg_i5{};
    std::string awg_j1{};
    std::string awg_j2{};
    std::string awg_j3{};
    std::int64_t itime{};
    std::string header_protection_key{};
    std::string content_padding_addition{};
    std::string rekey_after_time{};
    std::string rekey_timeout{};
    std::string reject_after_time{};
    std::string keepalive_timeout{};
    std::string max_handshake_attempts{};
    bool random_trailers{};
    bool disable_cookies{};
};

// WireGuard 对端配置
struct wireguard_peer_param {
    std::string server{};
    std::int64_t port{};
    std::string public_key{};
    std::string pre_shared_key{};
    std::vector<std::byte> reserved{};
    std::vector<std::string> allowed_ips{};
};

// WireGuard 专有参数（支持 AmneziaWG 混淆扩展）
struct wireguard_param {
    static constexpr protocol_type protocol_tag{protocol_type::wireguard};

    std::string public_key{};
    std::string pre_shared_key{};
    std::vector<std::byte> reserved{};
    std::vector<std::string> allowed_ips{};
    std::string ipv4_address{};
    std::string ipv6_address{};
    std::string private_key{};
    std::int64_t workers{};
    std::int64_t mtu{};
    std::int64_t persistent_keepalive{};
    ip_stack_config ip_stack{};
    std::optional<amnezia_wg_param> amnezia_wg_option{};
    std::vector<wireguard_peer_param> peers{};
    bool remote_dns_resolve{};
    std::vector<std::string> dns{};
    std::int64_t refresh_server_ip_interval{};
};

// TUIC 协议专有参数
struct tuic_param {
    static constexpr protocol_type protocol_tag{protocol_type::tuic};

    std::string token{};
    std::string uuid{};
    std::string password{};
    std::string ipv4_address{};
};

// ShadowQUIC 专有参数
struct shadowquic_param {
    static constexpr protocol_type protocol_tag{protocol_type::shadowquic};

    std::string username{};
    std::string password{};
};

// Gost 转发中继专有参数
struct gost_relay_param {
    static constexpr protocol_type protocol_tag{protocol_type::gost_relay};

    bool forward{};
    bool mux{};
    std::string username{};
    std::string password{};
};

// 直连出站
struct direct_param {
    static constexpr protocol_type protocol_tag{protocol_type::direct};
};

// DNS 解析出站
struct dns_param {
    static constexpr protocol_type protocol_tag{protocol_type::dns};
};

// 拒绝/拦截出站
struct reject_param {
    static constexpr protocol_type protocol_tag{protocol_type::reject};
};

// 重匹配出站
struct rematch_param {
    static constexpr protocol_type protocol_tag{protocol_type::rematch};

    std::optional<std::string> target_rematch_name{};
    std::optional<std::string> target_sub_rule{};
};

// SSH 隧道出站专有参数
struct ssh_param {
    static constexpr protocol_type protocol_tag{protocol_type::ssh};

    std::string username{};
    std::string password{};
    std::string private_key{};
    std::string private_key_passphrase{};
    std::vector<std::string> host_key{};
    std::vector<std::string> host_key_algorithms{};
};

// Mieru 协议专有参数
struct mieru_param {
    static constexpr protocol_type protocol_tag{protocol_type::mieru};

    std::string port_range{};
    std::string transport{};
    std::string username{};
    std::string password{};
    std::string multiplexing{};
    std::string handshake_mode{};
    std::string traffic_pattern{};
};

// AnyTLS 协议专有参数
struct anytls_param {
    static constexpr protocol_type protocol_tag{protocol_type::anytls};

    std::string password{};
    std::string client_metadata{};
    std::int64_t idle_session_check_interval{};
    std::int64_t idle_session_timeout{};
    std::int64_t min_idle_session{};
    bool disable_reuse{};
};

// Sudoku HTTP 伪装掩码选项
struct sudoku_http_mask_options {
    bool disable{};
    std::string mode{};
    bool tls{};
    std::string host{};
    std::string path_root{};
    std::string multiplex{};
};

// Sudoku 协议专有参数
struct sudoku_param {
    static constexpr protocol_type protocol_tag{protocol_type::sudoku};

    std::string key_material{};
    std::string aead_method{};
    std::optional<std::int64_t> padding_min{};
    std::optional<std::int64_t> padding_max{};
    std::string table_type{};
    std::optional<bool> enable_pure_downlink{};
    std::optional<bool> http_mask{};
    std::string http_mask_mode{};
    bool http_mask_tls{};
    std::string http_mask_host{};
    std::string path_root{};
    std::string multiplex{};
    std::string http_mask_multiplex{};
    std::optional<sudoku_http_mask_options> httpmask{};
    std::string custom_table{};
    std::vector<std::string> custom_tables{};
};

// MASQUE 协议专有参数
struct masque_param {
    static constexpr protocol_type protocol_tag{protocol_type::masque};

    std::string private_key{};
    std::string public_key{};
    std::string ipv4_address{};
    std::string ipv6_address{};
    std::string uri{};
    std::int64_t mtu{};
    std::string network{};
    ip_stack_config ip_stack{};
    bool remote_dns_resolve{};
    std::vector<std::string> dns{};
};

// TrustTunnel 协议专有参数
struct trusttunnel_param {
    static constexpr protocol_type protocol_tag{protocol_type::trusttunnel};

    std::string username{};
    std::string password{};
    bool health_check{};
    bool quic{};
    std::int64_t max_connections{};
    std::int64_t min_streams{};
    std::int64_t max_streams{};
};

// OpenVPN 客户端专有参数
struct openvpn_param {
    static constexpr protocol_type protocol_tag{protocol_type::openvpn};

    std::string proto{};
    std::string dev{};
    std::string cipher{};
    std::vector<std::string> data_ciphers{};
    std::string data_ciphers_fallback{};
    std::string auth{};
    std::string comp_lzo{};
    std::string ca_certificate{};
    std::string client_certificate{};
    std::string key_material{};
    std::string tls_auth{};
    std::string key_direction{};
    std::string tls_crypt{};
    std::string tls_crypt_v2{};
    std::string username{};
    std::string password{};
    string_map peer_info{};
    std::int64_t ping{};
    std::int64_t ping_restart{};
    std::optional<std::int64_t> tran_window{};
    std::int64_t mtu{};
    ip_stack_config ip_stack{};
    bool remote_dns_resolve{};
    std::vector<std::string> dns{};
};

// Tailscale 虚拟网专有参数
struct tailscale_param {
    static constexpr protocol_type protocol_tag{protocol_type::tailscale};

    std::string hostname{};
    std::string auth_key{};
    std::string control_url{};
    std::string state_dir{};
    bool ephemeral{};
    std::optional<bool> accept_routes{};
    std::string exit_node{};
    std::optional<bool> exit_node_allow_lan_access{};
};

// ZeroTier Orbit 选项
struct zero_tier_orbit_option {
    std::string world{};
    std::string seed{};
};

// ZeroTier 虚拟网专有参数
struct zerotier_param {
    static constexpr protocol_type protocol_tag{protocol_type::zerotier};

    std::string network{};
    std::string state_dir{};
    std::string identity_secret{};
    std::string planet{};
    std::int64_t mtu{};
    ip_stack_config ip_stack{};
    std::int64_t physical_mtu{};
    bool remote_dns_resolve{};
    std::vector<std::string> dns{};
    bool low_bandwidth{};
    bool encrypted_hello{};
    std::int64_t primary_port{};
    std::int64_t secondary_port{};
    std::string tcp_fallback_mode{};
    std::string tcp_fallback_relay{};
    std::vector<zero_tier_orbit_option> orbit{};
    std::string remote_trace_target{};
    std::int64_t remote_trace_level{};
};

// EasyTier 虚拟网专有参数
struct easytier_param {
    static constexpr protocol_type protocol_tag{protocol_type::easytier};

    std::string network_name{};
    std::string network_secret{};
    std::string hostname{};
    std::string ipv4{};
    bool dhcp{};
    std::vector<std::string> peers{};
    std::vector<std::string> listeners{};
    std::optional<bool> no_listener{};
    std::vector<std::string> mapped_listeners{};
    std::vector<std::string> exit_nodes{};
    std::vector<std::string> proxy_networks{};
    std::string instance_name{};
    std::string state_dir{};
    std::optional<bool> accept_dns{};
    std::optional<bool> enable_exit_node{};
    std::optional<bool> enable_encryption{};
    std::string encryption_algorithm{};
    std::optional<bool> private_mode{};
    std::optional<bool> latency_first{};
    std::optional<bool> disable_p2p{};
    std::optional<bool> enable_kcp_proxy{};
    std::optional<bool> disable_kcp_input{};
    std::optional<bool> enable_quic_proxy{};
    std::optional<bool> disable_quic_input{};
    std::int64_t mtu{};
    std::string tld_dns_zone{};
    std::optional<bool> secure_mode{};
    std::string local_private_key{};
    std::string local_public_key{};
};

// 协议专有参数变体联合体（覆盖 28 种出站协议，空间复用）
using protocol_detail =
    std::variant<std::monostate, shadowsocks_param, shadowsocksr_param, socks5_param, http_param,
                 vmess_param, vless_param, snell_param, trojan_param, hysteria_param,
                 hysteria2_param, wireguard_param, tuic_param, shadowquic_param, gost_relay_param,
                 direct_param, dns_param, reject_param, rematch_param, ssh_param, mieru_param,
                 anytls_param, sudoku_param, masque_param, trusttunnel_param, openvpn_param,
                 tailscale_param, zerotier_param, easytier_param>;

// 通过节点的 const 所有权指针访问的冷配置块（第二层链路控制 + 第三层专有凭证）
struct node_configuration {
    link_control link{};
    protocol_detail detail{};
};

// 第 4
// 层：客户端管理元数据（高频访问的热数据，禁止发给内核；冷热分离为设计访问分类，实际收益需经性能测试确认）
enum class probe_status : std::int8_t { never_tested, testing, reachable, timed_out, failed };

// 客户端运行时元数据（哈希存储槽、国家代码、测速状态、延迟与倍率等；哈希由外部计算提供，本模型不自动保证唯一性）
struct client_metadata {
    static constexpr int hash_byte_count{32};

    std::array<std::byte, hash_byte_count> unique_hash{};
    std::array<char, 2> country_code{};
    probe_status status{probe_status::never_tested};
    std::optional<std::chrono::milliseconds> latency{};
    double multiplier{1.0};
    std::optional<std::chrono::system_clock::time_point> last_probe{};
    std::optional<std::int64_t> download_bytes_per_second{};
};

// 统一节点模型（内嵌核心与元数据热数据，独占持有只读冷配置）
struct unified_node {
    universal_core core{};
    client_metadata metadata{};
    std::unique_ptr<const node_configuration> configuration{};
};

// 协议参数 Concept 约束
template <typename Parameter>
concept protocol_parameter = std::same_as<Parameter, std::monostate> || requires {
    { Parameter::protocol_tag } -> std::convertible_to<protocol_type>;
};

// 从参数变体中安全提取协议枚举类型（保证无异常抛出）
[[nodiscard]] constexpr auto
protocol_of(const protocol_detail& detail) noexcept // NOLINT(bugprone-exception-escape)
    -> protocol_type {
    if (detail.valueless_by_exception()) {
        return protocol_type::unspecified;
    }
    return std::visit(
        []<protocol_parameter Parameter>(const Parameter&) constexpr noexcept -> protocol_type {
            if constexpr (std::same_as<Parameter, std::monostate>) {
                return protocol_type::unspecified;
            } else {
                return Parameter::protocol_tag;
            }
        },
        detail);
}

// 校验核心层协议类型与凭证变体类型一致性（仅做类型匹配，默认未配置状态亦可能返回 true）
[[nodiscard]] inline auto has_matching_protocol(const unified_node& node) noexcept -> bool {
    if (!node.configuration) {
        return node.core.protocol == protocol_type::unspecified;
    }
    return !node.configuration->detail.valueless_by_exception() &&
           node.core.protocol == protocol_of(node.configuration->detail);
}

// 获取协议在 Mihomo 内核中的标准字符串名称
[[nodiscard]] constexpr auto kernel_protocol_name(protocol_type protocol) noexcept
    -> std::string_view {
    switch (protocol) {
    case protocol_type::shadowsocks:
        return "ss";
    case protocol_type::shadowsocksr:
        return "ssr";
    case protocol_type::socks5:
        return "socks5";
    case protocol_type::http:
        return "http";
    case protocol_type::vmess:
        return "vmess";
    case protocol_type::vless:
        return "vless";
    case protocol_type::snell:
        return "snell";
    case protocol_type::trojan:
        return "trojan";
    case protocol_type::hysteria:
        return "hysteria";
    case protocol_type::hysteria2:
        return "hysteria2";
    case protocol_type::wireguard:
        return "wireguard";
    case protocol_type::tuic:
        return "tuic";
    case protocol_type::shadowquic:
        return "shadowquic";
    case protocol_type::gost_relay:
        return "gost-relay";
    case protocol_type::direct:
        return "direct";
    case protocol_type::dns:
        return "dns";
    case protocol_type::reject:
        return "reject";
    case protocol_type::rematch:
        return "rematch";
    case protocol_type::ssh:
        return "ssh";
    case protocol_type::mieru:
        return "mieru";
    case protocol_type::anytls:
        return "anytls";
    case protocol_type::sudoku:
        return "sudoku";
    case protocol_type::masque:
        return "masque";
    case protocol_type::trusttunnel:
        return "trusttunnel";
    case protocol_type::openvpn:
        return "openvpn";
    case protocol_type::tailscale:
        return "tailscale";
    case protocol_type::zerotier:
        return "zerotier";
    case protocol_type::easytier:
        return "easytier";
    case protocol_type::unspecified:
        return {};
    }
    return {};
}

// 编译期静态架构与内存约束保证
static_assert(std::is_aggregate_v<unified_node>); // 必须是 C++20 聚合类型（支持聚合初始化）
static_assert(std::is_trivially_copyable_v<client_metadata>);      // 元数据必须可平凡复制
static_assert(std::is_nothrow_move_constructible_v<unified_node>); // 统一节点必须支持无异常移动构造
static_assert(
    !std::is_copy_constructible_v<unified_node>);    // 禁止隐式深拷贝（遵循零法则与独占所有权）
inline constexpr int protocol_alternative_count{29}; // 包含 monostate 与 28 种协议的变体分支总数
static_assert(std::cmp_equal(std::variant_size_v<protocol_detail>,
                             protocol_alternative_count));        // 检查变体分支总数与预期一致
static_assert(sizeof(unified_node) < sizeof(node_configuration)); // 检查节点本体尺寸小于冷配置对象

// NOLINTEND(altera-struct-pack-align, readability-redundant-member-init)

} // namespace node_model

#endif // BLANK_QUICK_UNIFIED_NODE_HPP
