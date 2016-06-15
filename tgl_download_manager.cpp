#include "auto/auto-fetch-ds.h"
#include "auto/auto-free-ds.h"
#include "auto/auto-skip.h"
#include "tgl_download_manager.h"
#include "queries.h"
#include "tg-mime-types.h"
#include "tgl-layout.h"
#include "crypto/aes.h"
#include "crypto/md5.h"
#include "mtproto-common.h"
#include "tools.h"
#include "types/tgl_secret_chat.h"
#include "types/tgl_update_callback.h"
#include "queries-encrypted.h"

#include <fcntl.h>
#include <boost/filesystem.hpp>
#include <fstream>

static constexpr int BIG_FILE_THRESHOLD = 16 * 1024 * 1024;

struct send_file {
    int fd;
    long long size;
    long long offset;
    int part_num;
    int part_size;
    long long id;
    long long thumb_id;
    tgl_peer_id_t to_id;
    int flags;
    std::string file_name;
    bool encr;
    int avatar;
    int reply;
    std::array<unsigned char, 32> iv;
    std::array<unsigned char, 32> init_iv;
    std::array<unsigned char, 32> key;
    int w;
    int h;
    int duration;
    std::string caption;

    std::vector<char> thumb;
    int thumb_w;
    int thumb_h;

    tgl_message_id_t message_id;

    std::vector<char> sending_buffer;

    send_file()
        : fd(-1)
        , size(0)
        , offset(0)
        , part_num(0)
        , part_size(0)
        , id(0)
        , thumb_id(0)
        , to_id()
        , flags(0)
        , encr(false)
        , avatar(0)
        , reply(0)
        , w(0)
        , h(0)
        , duration(0)
        , thumb_w(0)
        , thumb_h(0)
    { }

    ~send_file()
    {
        // For security reasion.
        memset(iv.data(), 0, iv.size());
        memset(init_iv.data(), 0, init_iv.size());
        memset(key.data(), 0, key.size());
    }
};

struct download {
    download(int size, const tgl_file_location& location)
        : location(location), offset(0), size(size), fd(-1), iv(), key(), type(0), refcnt(0)
    {
    }

    download(int type, const std::shared_ptr<tgl_document>&);

    ~download()
    {
        memset(iv.data(), 0, iv.size());
        memset(key.data(), 0, key.size());
    }

    tgl_file_location location;
    int offset;
    int size;
    int fd;
    std::string name;
    std::string ext;
    //encrypted documents
    std::vector<unsigned char> iv;
    std::vector<unsigned char> key;
    // ---
    int type;
    int refcnt; //Probably intended for being able to load multiple file parts simultaniously...however downloading is done sequentially
};

class query_send_file_part: public query
{
public:
    query_send_file_part(tgl_download_manager* download_manager,
            const std::shared_ptr<send_file>& file,
            const tgl_upload_callback& callback)
        : query("send file part", TYPE_TO_PARAM(bool))
        , m_download_manager(download_manager)
        , m_file(file)
        , m_callback(callback)
    { }

    const std::shared_ptr<query_send_file_part> shared_from_this()
    {
        return std::static_pointer_cast<query_send_file_part>(query::shared_from_this());
    }

    virtual void on_answer(void* answer) override
    {
        m_download_manager->send_file_part_on_answer(shared_from_this(), answer);
    }

    virtual int on_error(int error_code, const std::string& error_string) override
    {
        TGL_ERROR("RPC_CALL_FAIL " << error_code << error_string);
        if (m_callback) {
            m_callback(false, nullptr, 0);
        }
        return 0;
    }

    const tgl_upload_callback& callback() const
    {
        return m_callback;
    }

    const std::shared_ptr<send_file>& file() const
    {
        return m_file;
    }

private:
    tgl_download_manager* m_download_manager;
    std::shared_ptr<send_file> m_file;
    tgl_upload_callback m_callback;
};

class query_set_photo: public query
{
public:
    explicit query_set_photo(const std::function<void(bool)>& callback)
        : query("set photo", TYPE_TO_PARAM(photos_photo))
        , m_callback(callback)
    { }

    const std::shared_ptr<query_set_photo> shared_from_this()
    {
        return std::static_pointer_cast<query_set_photo>(query::shared_from_this());
    }

    virtual void on_answer(void*) override
    {
        if (m_callback) {
            m_callback(true);
        }
    }

    virtual int on_error(int error_code, const std::string& error_string) override
    {
        TGL_ERROR("set photo error: " << error_code << " " << error_string);
        if (m_callback) {
            m_callback(false);
        }
        return 0;
    }

private:
    std::function<void(bool)> m_callback;
};

class query_download: public query
{
public:
    query_download(tgl_download_manager* download_manager,
            const std::shared_ptr<download>& download,
            const tgl_download_callback& callback)
        : query("download", TYPE_TO_PARAM(upload_file))
        , m_download_manager(download_manager)
        , m_download(download)
        , m_callback(callback)
    { }

    const std::shared_ptr<query_download> shared_from_this()
    {
        return std::static_pointer_cast<query_download>(query::shared_from_this());
    }

    virtual void on_answer(void* answer) override
    {
        m_download_manager->download_on_answer(shared_from_this(), answer);
    }

    virtual int on_error(int error_code, const std::string& error_string) override
    {
        return m_download_manager->download_on_error(shared_from_this(), error_code, error_string);
    }

    const tgl_download_callback& callback() const
    {
        return m_callback;
    }

    const std::shared_ptr<download>& get_download() const
    {
        return m_download;
    }

private:
    tgl_download_manager* m_download_manager;
    std::shared_ptr<download> m_download;
    tgl_download_callback m_callback;
};

class query_send_encr_file: public query
{
public:
    query_send_encr_file(
            const std::shared_ptr<tgl_secret_chat>& secret_chat,
            const std::shared_ptr<tgl_message>& message,
            const std::function<void(bool, const std::shared_ptr<tgl_message>&, float)>& callback)
        : query("send encrypted (file)", TYPE_TO_PARAM(messages_sent_encrypted_message))
        , m_secret_chat(secret_chat)
        , m_message(message)
        , m_callback(callback)
    { }

    void set_message(const std::shared_ptr<tgl_message>& message)
    {
        m_message = message;
    }

    virtual void on_answer(void* D) override
    {
        tl_ds_messages_sent_encrypted_message* DS_MSEM = static_cast<tl_ds_messages_sent_encrypted_message*>(D);

        tglm_edit_encr_message(m_message, nullptr, nullptr, DS_MSEM->date, std::string(), nullptr, nullptr, DS_MSEM->file, m_message->flags & (~TGLMF_PENDING));
        tgl_state::instance()->callback()->new_messages({m_message});

        if (m_callback) {
            m_callback(true, m_message, 0);
        }

        tgl_state::instance()->callback()->message_sent(m_message, m_message->permanent_id.id, m_secret_chat->out_seq_no);
    }

    virtual int on_error(int error_code, const std::string& error_string) override
    {
        if (m_secret_chat && m_secret_chat->state != sc_deleted && error_code == 400 && error_string == "ENCRYPTION_DECLINED") {
            tgl_secret_chat_deleted(m_secret_chat);
        }

        if (m_callback) {
            m_callback(false, m_message, 0);
        }

        if (m_message) {
            //bl_do_message_delete (&M->permanent_id);
            // FIXME: is this correct?
            tgl_state::instance()->callback()->message_deleted(m_message->permanent_id.id);
        }
        return 0;
    }

private:
    std::shared_ptr<tgl_secret_chat> m_secret_chat;
    std::shared_ptr<tgl_message> m_message;
    std::function<void(bool, const std::shared_ptr<tgl_message>&, float)> m_callback;
};

download::download(int type, const std::shared_ptr<tgl_document>& document)
    : offset(0)
    , size(document->size)
    , fd(-1)
    , type(type)
    , refcnt(0)
{
    location.set_dc(document->dc_id);
    location.set_local_id(0);
    location.set_secret(document->access_hash);
    location.set_volume(document->id);
}

tgl_download_manager::tgl_download_manager(std::string download_directory)
    : m_download_directory(download_directory)
    , m_current_uploading_bytes(0)
    , m_current_uploaded_bytes(0)
    , m_current_downloading_bytes(0)
    , m_current_downloaded_bytes(0)
{
}

bool tgl_download_manager::file_exists(const tgl_file_location &location)
{
    std::string path = get_file_path(location.access_hash());
    return boost::filesystem::exists(path);
}

bool tgl_download_manager::currently_donwloading(const tgl_file_location& location)
{
    for (auto it=m_downloads.begin(); it!= m_downloads.end(); it++) {
        if ((*it)->location.secret() == location.secret()) {
            return true;
        }
    }
    return false;
}

std::string tgl_download_manager::get_file_path(long long int secret)
{
    std::ostringstream stream;
    stream << download_directory() << "/download_" << secret;
    return stream.str();
}

int tgl_download_manager::send_file_part_on_answer(const std::shared_ptr<query_send_file_part>& q, void*)
{
    send_part(q->file(), q->callback());
    return 0;
}

void tgl_download_manager::send_avatar_end(const std::shared_ptr<send_file>& f, const std::function<void(bool)>& callback)
{
    if (f->avatar > 0) {
        auto q = std::make_shared<query_send_msgs>(callback);
        q->out_i32(CODE_messages_edit_chat_photo);
        q->out_i32(f->avatar);
        q->out_i32(CODE_input_chat_uploaded_photo);
        if (f->size < BIG_FILE_THRESHOLD) {
            q->out_i32(CODE_input_file);
        } else {
            q->out_i32(CODE_input_file_big);
        }
        q->out_i64(f->id);
        q->out_i32(f->part_num);
        q->out_string("");
        if (f->size < BIG_FILE_THRESHOLD) {
            q->out_string("");
        }
        q->out_i32(CODE_input_photo_crop_auto);

        q->execute(tgl_state::instance()->working_dc());
    } else {
        auto q = std::make_shared<query_set_photo>(callback);
        q->out_i32(CODE_photos_upload_profile_photo);
        if (f->size < BIG_FILE_THRESHOLD) {
            q->out_i32(CODE_input_file);
        } else {
            q->out_i32(CODE_input_file_big);
        }
        q->out_i64(f->id);
        q->out_i32(f->part_num);
        boost::filesystem::path path(f->file_name);
        q->out_std_string(path.filename().string());
        if (f->size < BIG_FILE_THRESHOLD) {
            q->out_string("");
        }
        q->out_string("profile photo");
        q->out_i32(CODE_input_geo_point_empty);
        q->out_i32(CODE_input_photo_crop_auto);

        q->execute(tgl_state::instance()->working_dc());
    }
}


void tgl_download_manager::send_unencrypted_file_end(const std::shared_ptr<send_file>& f,
        const tgl_upload_callback& callback)
{
    std::shared_ptr<messages_send_extra> E = std::make_shared<messages_send_extra>();
    E->id = f->message_id;
    auto q = std::make_shared<query_send_msgs>(E, callback);

    auto message = std::make_shared<tgl_message>();
    message->permanent_id = f->message_id;
    message->to_id = f->to_id;
    message->from_id = tgl_state::instance()->our_id();
    q->set_message(message);

    q->out_i32(CODE_messages_send_media);
    q->out_i32((f->reply ? 1 : 0));
    q->out_peer_id(f->to_id);
    if (f->reply) {
        q->out_i32(f->reply);
    }
    if (f->flags & TGL_SEND_MSG_FLAG_DOCUMENT_PHOTO) {
        q->out_i32(CODE_input_media_uploaded_photo);
    } else {
        if (f->thumb_id > 0) {
            q->out_i32(CODE_input_media_uploaded_thumb_document);
        } else {
            q->out_i32(CODE_input_media_uploaded_document);
        }
    }

    if (f->size < BIG_FILE_THRESHOLD) {
        q->out_i32(CODE_input_file);
    } else {
        q->out_i32(CODE_input_file_big);
    }

    q->out_i64(f->id);
    q->out_i32(f->part_num);
    std::string file_name = boost::filesystem::path(f->file_name).filename().string();
    q->out_std_string(file_name);
    if (f->size < BIG_FILE_THRESHOLD) {
        q->out_string("");
    }

    if (!(f->flags & TGL_SEND_MSG_FLAG_DOCUMENT_PHOTO)) {

        if (f->thumb_id > 0) {
            q->out_i32(CODE_input_file);
            q->out_i64(f->thumb_id);
            q->out_i32(1);
            q->out_string("thumb.jpg");
            q->out_string("");
        }

        q->out_string(tg_mime_by_filename(f->file_name.c_str()));

        q->out_i32(CODE_vector);
        if (f->flags & TGLDF_IMAGE) {
            if (f->flags & TGLDF_ANIMATED) {
                q->out_i32(2);
                q->out_i32(CODE_document_attribute_image_size);
                q->out_i32(f->w);
                q->out_i32(f->h);
                q->out_i32(CODE_document_attribute_animated);
            } else {
                q->out_i32(1);
                q->out_i32(CODE_document_attribute_image_size);
                q->out_i32(f->w);
                q->out_i32(f->h);
            }
        } else if (f->flags & TGLDF_AUDIO) {
            q->out_i32(2);
            q->out_i32(CODE_document_attribute_audio);
            q->out_i32(f->duration);
            q->out_i32(CODE_document_attribute_filename);
            q->out_std_string(file_name);
        } else if (f->flags & TGLDF_VIDEO) {
            q->out_i32(2);
            q->out_i32(CODE_document_attribute_video);
            q->out_i32(f->duration);
            q->out_i32(f->w);
            q->out_i32(f->h);
            q->out_i32(CODE_document_attribute_filename);
            q->out_std_string(file_name);
        } else if (f->flags & TGLDF_STICKER) {
            q->out_i32(1);
            q->out_i32(CODE_document_attribute_sticker);
        } else {
            q->out_i32(1);
            q->out_i32(CODE_document_attribute_filename);
            q->out_std_string(file_name);
        }

        q->out_std_string(f->caption);
    } else {
        q->out_std_string(f->caption);
    }

    q->out_i64(E->id.id);

    q->execute(tgl_state::instance()->working_dc());
}

void tgl_download_manager::send_encrypted_file_end(const std::shared_ptr<send_file>& f,
        const tgl_upload_callback& callback)
{
    std::shared_ptr<tgl_secret_chat> secret_chat = tgl_state::instance()->secret_chat_for_id(f->to_id);
    assert(secret_chat);
    auto q = std::make_shared<query_send_encr_file>(secret_chat, nullptr, callback);
    secret_chat_encryptor encryptor(secret_chat, q->serializer());
    q->out_i32(CODE_messages_send_encrypted_file);
    q->out_i32(CODE_input_encrypted_chat);
    q->out_i32(f->to_id.peer_id);
    q->out_i64(secret_chat->access_hash);
    long long r;
    tglt_secure_random (reinterpret_cast<unsigned char*>(&r), 8);
    q->out_i64(r);
    encryptor.start();
    q->out_i32(CODE_decrypted_message_layer);
    q->out_random (15 + 4 * (rand () % 3));
    q->out_i32(TGL_ENCRYPTED_LAYER);
    q->out_i32(2 * secret_chat->in_seq_no + (secret_chat->admin_id != tgl_state::instance()->our_id().peer_id));
    q->out_i32(2 * secret_chat->out_seq_no + (secret_chat->admin_id == tgl_state::instance()->our_id().peer_id));
    q->out_i32(CODE_decrypted_message);
    q->out_i64(r);
    q->out_i32(secret_chat->ttl);
    q->out_string("");

    size_t start = q->serializer()->i32_size();

    if (f->flags & TGL_SEND_MSG_FLAG_DOCUMENT_PHOTO) {
       q->out_i32(CODE_decrypted_message_media_photo);
    } else if ((f->flags & TGLDF_VIDEO)) {
       q->out_i32(CODE_decrypted_message_media_video);
    } else if ((f->flags & TGLDF_AUDIO)) {
       q->out_i32(CODE_decrypted_message_media_audio);
    } else {
       q->out_i32(CODE_decrypted_message_media_document);
    }
    if ((f->flags & TGL_SEND_MSG_FLAG_DOCUMENT_PHOTO) || !(f->flags & TGLDF_AUDIO)) {
        q->out_string(f->thumb.data(), f->thumb.size());
        q->out_i32(f->thumb_w);
        q->out_i32(f->thumb_h);
    }

    if (f->flags & TGL_SEND_MSG_FLAG_DOCUMENT_PHOTO) {
        q->out_i32(f->w);
        q->out_i32(f->h);
    } else if (f->flags & TGLDF_VIDEO) {
        q->out_i32(f->duration);
        q->out_string(tg_mime_by_filename(f->file_name.c_str()));
        q->out_i32(f->w);
        q->out_i32(f->h);
    } else if (f->flags & TGLDF_AUDIO) {
        q->out_i32(f->duration);
        q->out_string(tg_mime_by_filename(f->file_name.c_str()));
    } else { // document
        boost::filesystem::path path(f->file_name);
        q->out_std_string(path.filename().string());
        q->out_string(tg_mime_by_filename(f->file_name.c_str()));
    }

    q->out_i32(f->size);
    q->out_string(reinterpret_cast<const char*>(f->key.data()), f->key.size());
    q->out_string(reinterpret_cast<const char*>(f->init_iv.data()), f->init_iv.size());

    tgl_in_buffer in = { q->serializer()->mutable_i32_data() + start, q->serializer()->mutable_i32_data() + q->serializer()->i32_size() };

    struct paramed_type decrypted_message_media = TYPE_TO_PARAM(decrypted_message_media);
    auto result = skip_type_any(&in, &decrypted_message_media);
    TGL_ASSERT_UNUSED(result, result >= 0);
    assert(in.ptr == in.end);

    in = { q->serializer()->mutable_i32_data() + start, q->serializer()->mutable_i32_data() + q->serializer()->i32_size() };
    tl_ds_decrypted_message_media* DS_DMM = fetch_ds_type_decrypted_message_media(&in, &decrypted_message_media);
    assert(in.ptr == in.end);

    encryptor.end();

    if (f->size < BIG_FILE_THRESHOLD) {
        q->out_i32(CODE_input_encrypted_file_uploaded);
    } else {
        q->out_i32(CODE_input_encrypted_file_big_uploaded);
    }
    q->out_i64(f->id);
    q->out_i32(f->part_num);
    if (f->size < BIG_FILE_THRESHOLD) {
        q->out_string ("");
    }

    unsigned char md5[16];
    unsigned char str[64];
    memcpy(str, f->key.data(), 32);
    memcpy(str + 32, f->init_iv.data(), 32);
    TGLC_md5(str, 64, md5);
    q->out_i32((*(int *)md5) ^ (*(int *)(md5 + 4)));

    tgl_peer_id_t from_id = tgl_state::instance()->our_id();

    int date = time(NULL);
    std::shared_ptr<tgl_message> message = tglm_create_encr_message(&f->message_id,
        &from_id,
        &f->to_id,
        &date,
        std::string(),
        DS_DMM,
        NULL,
        NULL,
        TGLMF_OUT | TGLMF_UNREAD | TGLMF_ENCRYPTED | TGLMF_CREATE | TGLMF_CREATED);
    free_ds_type_decrypted_message_media (DS_DMM, &decrypted_message_media);

    if (message->media->type() == tgl_message_media_type_document_encr) {
        if (auto encr_document = std::static_pointer_cast<tgl_message_media_document_encr>(message->media)->encr_document) {
            if (f->flags & TGL_SEND_MSG_FLAG_DOCUMENT_PHOTO) {
                encr_document->flags |= TGLDF_IMAGE;
            }
            if (f->flags & TGLDF_VIDEO) {
                encr_document->flags |= TGLDF_VIDEO;
            }
            if (f->flags & TGLDF_AUDIO) {
                encr_document->flags |= TGLDF_AUDIO;
            }
        }
    }

    q->set_message(message);
    q->execute(tgl_state::instance()->working_dc());
}

void tgl_download_manager::send_file_end(const std::shared_ptr<send_file>& f,
        const tgl_upload_callback& callback)
{
    TGL_NOTICE("send_file_end");
    m_current_uploaded_bytes -= f->size;
    m_current_uploading_bytes -= f->size;

    if (f->avatar) {
        send_avatar_end (f,
                [=](bool success) {
                    if(callback) {
                        callback(success, nullptr, 0);
                    }
                });
        return;
    }
    if (!f->encr) {
        TGL_NOTICE("send_file_end - send_unencrypted_file_end");
        send_unencrypted_file_end(f, callback);
        return;
    }
    TGL_NOTICE("send_file_end - send_encrypted_file_end");
    send_encrypted_file_end(f, callback);
    return;
}

void tgl_download_manager::send_part(const std::shared_ptr<send_file>& f,
        const tgl_upload_callback& callback)
{
    if (f->fd >= 0) {
        if (!f->part_num) {
            m_current_uploading_bytes += f->size;
        }
        auto q = std::make_shared<query_send_file_part>(this, f, callback);
        if (f->size < BIG_FILE_THRESHOLD) {
            q->out_i32(CODE_upload_save_file_part);
            q->out_i64(f->id);
            q->out_i32(f->part_num ++);
        } else {
            q->out_i32(CODE_upload_save_big_file_part);
            q->out_i64(f->id);
            q->out_i32(f->part_num ++);
            q->out_i32((f->size + f->part_size - 1) / f->part_size);
        }

        if (f->sending_buffer.empty()) {
            f->sending_buffer.resize(f->part_size);
        }

        int read_size = 0;
        assert(f->part_size > 0);
        while (read_size < f->part_size) {
            ssize_t ret = read(f->fd, f->sending_buffer.data() + read_size, f->part_size - read_size);
            if (ret < 0) {
                if (ret == EINTR) {
                    continue;
                }
                if (callback) {
                    callback(false, nullptr, 0);
                }
                return;
            } else if (ret == 0) {
                break;
            }
            read_size += ret;
        }

        if (read_size == 0) {
            TGL_WARNING("could not send empty file");
            if (callback) {
                callback(false, nullptr, 0);
            }
            return;
        }

        assert(read_size > 0);
        f->offset += read_size;
        m_current_uploaded_bytes += read_size;

        if (f->encr) {
            if (read_size & 15) {
                assert(f->offset == f->size);
                tglt_secure_random(reinterpret_cast<unsigned char*>(f->sending_buffer.data()) + read_size, (-read_size) & 15);
                read_size = (read_size + 15) & ~15;
            }

            TGLC_aes_key aes_key;
            TGLC_aes_set_encrypt_key(f->key.data(), 256, &aes_key);
            TGLC_aes_ige_encrypt(reinterpret_cast<unsigned char*>(f->sending_buffer.data()),
                    reinterpret_cast<unsigned char*>(f->sending_buffer.data()), read_size, &aes_key, f->iv.data(), 1);
            memset(&aes_key, 0, sizeof(aes_key));
        }
        q->out_string(f->sending_buffer.data(), read_size);
        TGL_DEBUG("offset=" << f->offset << " size=" << f->size);

        if (callback) {
            float progress = (float)f->offset/(float)f->size;
            callback(false, nullptr, progress);
        }

        if (f->offset == f->size) {
            f->sending_buffer.clear();
            close(f->fd);
            f->fd = -1;
        } else {
            assert(f->part_size == read_size);
        }
        q->execute(tgl_state::instance()->working_dc());
    } else {
        send_file_end(f, callback);
    }
}

void tgl_download_manager::send_file_thumb(const std::shared_ptr<send_file>& f,
        const tgl_upload_callback& callback)
{
    TGL_NOTICE("send_file_thumb");

    auto q = std::make_shared<query_send_file_part>(this, f, callback);
    f->thumb_id = lrand48 () * (1ll << 32) + lrand48 ();
    q->out_i32(CODE_upload_save_file_part);
    q->out_i64(f->thumb_id);
    q->out_i32(0);
    q->out_string(f->thumb.data(), f->thumb.size());

    q->execute(tgl_state::instance()->working_dc());
}


void tgl_download_manager::send_document(const tgl_peer_id_t& to_id,
        const tgl_message_id_t& message_id, const std::string &file_name, int avatar, int w, int h, int duration,
        const std::string& caption, unsigned long long flags,
        const std::string& thumb_path, int thumb_w, int thumb_h,
        const tgl_upload_callback& callback)
{
    int fd = -1;
    if (!boost::filesystem::exists(file_name)) {
        TGL_ERROR("File " << file_name << " does not exist");
        return;
    }
    long long size = boost::filesystem::file_size(file_name);
    TGL_NOTICE("send_document " << file_name << " with size " << size << " and dimession " << w << " X " << h);
    if (size <= 0 || (fd = open (file_name.c_str(), O_RDONLY)) <= 0) {
        TGL_ERROR("file is empty");
        if (callback) {
            callback(false, nullptr, 0);
        }
        return;
    }

    std::shared_ptr<send_file> f = std::make_shared<send_file>();
    f->fd = fd;
    f->size = size;
    f->offset = 0;
    f->part_num = 0;
    f->avatar = avatar;
    f->message_id = message_id;
    f->reply = flags >> 32;
    f->part_size = 512 * 1024;
    f->flags = flags;

    static constexpr int MAX_PARTS = 3000; // How do we get this number?
    if (((size + f->part_size - 1) / f->part_size) > MAX_PARTS) {
        close(fd);
        TGL_ERROR("File is too big");
        if (callback) {
            callback(false, nullptr, 0);
        }
        return;
    }

    tglt_secure_random(reinterpret_cast<unsigned char*>(&f->id), 8);
    f->to_id = to_id;
    f->flags = flags;
    f->file_name = file_name;
    f->w = w;
    f->h = h;
    f->duration = duration;
    f->caption = caption;

    if (f->to_id.peer_type == tgl_peer_type::enc_chat) {
        f->encr = true;
        tglt_secure_random (f->iv.data(), f->iv.size());
        memcpy(f->init_iv.data(), f->iv.data(), f->iv.size());
        tglt_secure_random(f->key.data(), f->key.size());
    }

    if (boost::filesystem::exists(thumb_path)) {
        boost::system::error_code ec;
        auto file_size = boost::filesystem::file_size(thumb_path, ec);
        if (ec == 0) {
            TGL_NOTICE("thumbnail_path " << thumb_path << " with size " << file_size);
            std::ifstream ifs(thumb_path, std::ios_base::in | std::ios_base::binary);
            if (ifs.good()) {
                f->thumb.resize(file_size);
                ifs.read(f->thumb.data(), file_size);
                if (ifs.gcount() == static_cast<std::streamsize>(file_size)) {
                    f->thumb_w = thumb_w;
                    f->thumb_h = thumb_h;
                } else {
                    TGL_ERROR("failed to read thumbnail file: " << thumb_path);
                    f->thumb.clear();
                }
            }
        }
    }

    if (!f->encr && f->flags != -1 && f->thumb.size() > 0) {
        send_file_thumb(f, callback);
    } else {
        send_part(f, callback);
    }
}

void tgl_download_manager::set_chat_photo(tgl_peer_id_t chat_id, const std::string& file_name,
        const std::function<void(bool success)>& callback)
{
    assert(chat_id.peer_type == tgl_peer_type::chat);
    send_document(chat_id, tgl_message_id_t(), file_name, chat_id.peer_id,
            0, 0, 0, std::string(), TGL_SEND_MSG_FLAG_DOCUMENT_PHOTO, std::string(), 0 , 0,
            [=](bool success, const std::shared_ptr<tgl_message>&, float) {
                if (callback) {
                    callback(success);
                }
            });
}

void tgl_download_manager::set_profile_photo(const std::string& file_name,
        const std::function<void(bool success)>& callback)
{
    send_document(tgl_state::instance()->our_id(), tgl_message_id_t(), file_name, -1,
            0, 0, 0, std::string(), TGL_SEND_MSG_FLAG_DOCUMENT_PHOTO, std::string(), 0, 0,
            [=](bool success, const std::shared_ptr<tgl_message>&, float) {
                if (callback) {
                    callback(success);
                }
            });
}

void tgl_download_manager::send_document(const tgl_peer_id_t& to_id, const tgl_message_id_t& message_id,
        const std::string& file_name, int32_t width, int32_t height, int32_t duration, const std::string& caption,
        const std::string& thumb_path, int32_t thumb_width, int32_t thumb_height, unsigned long long flags,
        const tgl_upload_callback& callback)
{
    TGL_DEBUG("send_document - file_name: " + file_name);
    if (flags & TGL_SEND_MSG_FLAG_DOCUMENT_AUTO) {
        const char *mime_type = tg_mime_by_filename (file_name.c_str());
        TGL_DEBUG("send_document - detected mime_type: " + std::string(mime_type));
        if (strcmp(mime_type, "image/gif") == 0) {
            flags |= TGL_SEND_MSG_FLAG_DOCUMENT_ANIMATED;
        } else if (!memcmp(mime_type, "image/", 6)) {
            flags |= TGL_SEND_MSG_FLAG_DOCUMENT_PHOTO;
        } else if (!memcmp(mime_type, "video/", 6)) {
            flags |= TGLDF_VIDEO;
        } else if (!memcmp(mime_type, "audio/", 6)) {
            flags |= TGLDF_AUDIO;
        }
    }

    send_document(to_id, message_id, file_name, 0,
            width, height, duration, caption, flags, thumb_path, thumb_width, thumb_height, callback);
}

void tgl_download_manager::end_download(const std::shared_ptr<download>& d,
        const tgl_download_callback& callback)
{
    for (auto it = m_downloads.begin(); it != m_downloads.end(); ++it) {
        if (*it == d) {
            m_downloads.erase(it);
            break;
        }
    }

    m_current_downloading_bytes -= d->size;
    m_current_downloaded_bytes -= d->size;

    if (d->fd >= 0) {
        close (d->fd);
    }

    if (callback) {
        callback(true, d->name, 1);
    }
}

int tgl_download_manager::download_on_answer(const std::shared_ptr<query_download>& q, void *DD)
{
    tl_ds_upload_file* DS_UF = static_cast<tl_ds_upload_file*>(DD);

    const std::shared_ptr<download>& d = q->get_download();
    if (d->fd == -1) {
        d->fd = open (d->name.c_str(), O_CREAT | O_WRONLY, 0640);
        if (d->fd < 0) {
            TGL_ERROR("Can not open file for writing: %m");
            if (q->callback()) {
                (q->callback())(false, std::string(), 0);
            }

            return 0;
        }
    }

    int len = DS_UF->bytes->len;
    m_current_downloaded_bytes += len;

    if (!d->iv.empty()) {
        assert (!(len & 15));
        void *ptr = DS_UF->bytes->data;

        TGLC_aes_key aes_key;
        TGLC_aes_set_decrypt_key(d->key.data(), 256, &aes_key);
        TGLC_aes_ige_encrypt(static_cast<unsigned char*>(ptr), static_cast<unsigned char*>(ptr), len, &aes_key, d->iv.data(), 0);
        memset(&aes_key, 0, sizeof(aes_key));
        if (len > d->size - d->offset) {
            len = d->size - d->offset;
        }
        auto result = write(d->fd, ptr, len);
        TGL_ASSERT_UNUSED(result, result == len);
    } else {
        auto result = write(d->fd, DS_UF->bytes->data, len);
        TGL_ASSERT_UNUSED(result, result == len);
    }

    d->offset += len;
    d->refcnt--;
    if (d->offset < d->size) {
        float progress = static_cast<float>(d->offset)/d->size;
        (q->callback())(false, std::string(), progress);
        download_next_part(d, q->callback());
        return 0;
    } else {
        if (!d->refcnt) {
            end_download(d, q->callback());
        }
        return 0;
    }
}

int tgl_download_manager::download_on_error(const std::shared_ptr<query_download>& q, int error_code, const std::string &error)
{
    TGL_ERROR("RPC_CALL_FAIL " << error_code << " " << std::string(error));

    const std::shared_ptr<download>& d = q->get_download();
    if (d->fd >= 0) {
        close(d->fd);
    }

    if (q->callback()) {
        (q->callback())(false, d->name, 0);
    }

    return 0;
}

void tgl_download_manager::begin_download(const std::shared_ptr<download>& new_download)
{
    m_downloads.push_back(new_download);
}

void tgl_download_manager::download_next_part(const std::shared_ptr<download>& d,
        const tgl_download_callback& callback)
{
    TGL_DEBUG("download_next_part (file size " << d->size << ")");
    if (!d->offset) {
        std::string path = get_file_path(d->location.access_hash());

        if (!d->ext.empty()) {
            path += std::string(".") + d->ext;
        }

        d->name = path;
        if (boost::filesystem::exists(path)) {
            d->offset = boost::filesystem::file_size(path);
            if (d->offset >= d->size) {
                m_current_downloading_bytes += d->size;
                m_current_downloaded_bytes += d->offset;
                TGL_NOTICE("Already downloaded");
                end_download(d, callback);
                return;
            }
        }

        m_current_downloading_bytes += d->size;
        m_current_downloaded_bytes += d->offset;
    }
    d->refcnt++;
    auto q = std::make_shared<query_download>(this, d, callback);
    q->out_i32 (CODE_upload_get_file);
    if (d->location.local_id()) {
        q->out_i32(CODE_input_file_location);
        q->out_i64(d->location.volume());
        q->out_i32(d->location.local_id());
        q->out_i64(d->location.secret());
    } else {
        if (!d->iv.empty()) {
            q->out_i32(CODE_input_encrypted_file_location);
        } else {
            q->out_i32(d->type);
        }
        q->out_i64(d->location.document_id());
        q->out_i64(d->location.access_hash());
    }
    q->out_i32(d->offset);
    q->out_i32(512 * 1024);

    q->execute(tgl_state::instance()->dc_at(d->location.dc()));
}

void tgl_download_manager::download_by_photo_size(const std::shared_ptr<tgl_photo_size>& photo_size,
        const tgl_download_callback& callback)
{
    assert(photo_size);
    if (!photo_size->loc.dc()) {
        TGL_WARNING("bad video thumb");
        if (callback) {
            callback(false, std::string(), 0);
        }
        return;
    }

    std::shared_ptr<download> d = std::make_shared<download>(photo_size->size, photo_size->loc);
    download_next_part(d, callback);
}

void tgl_download_manager::download_by_file_location(const tgl_file_location& file_location, const int32_t file_size,
        const tgl_download_callback& callback)
{
    if (!file_location.dc()) {
        TGL_ERROR("Bad file location");
        if (callback) {
            callback(false, std::string(), 0);
        }
        return;
    }

    std::shared_ptr<download> d = std::make_shared<download>(file_size, file_location);
    TGL_DEBUG("download_file_location - file_size: " << file_size);
    download_next_part(d, callback);
}

void tgl_download_manager::download_photo(const std::shared_ptr<tgl_photo>& photo,
        const tgl_download_callback& callback)
{
    if (!photo->sizes.size()) {
        TGL_ERROR("bad photo (no photo sizes");
        if (callback) {
            callback(false, std::string(), 0);
        }
        return;
    }
    int max = -1;
    int maxi = 0;
    int i;
    for (i = 0; i < static_cast<int>(photo->sizes.size()); i++) {
        if (photo->sizes[i]->w + photo->sizes[i]->h > max) {
            max = photo->sizes[i]->w + photo->sizes[i]->h;
            maxi = i;
        }
    }
    download_by_photo_size(photo->sizes[maxi], callback);
}

void tgl_download_manager::download_document_thumb(const std::shared_ptr<tgl_document>& document,
        const tgl_download_callback& callback)
{
    download_by_photo_size(document->thumb, callback);
}

void tgl_download_manager::download_document(const std::shared_ptr<tgl_document>& document,
        const std::shared_ptr<download>& d,
        const tgl_download_callback& callback)
{
    assert(document);

    if (!document->mime_type.empty()) {
        const char* ext = tg_extension_by_mime(document->mime_type.c_str());
        if (ext) {
            d->ext = std::string(ext);
        }
    }
    begin_download(d);
    download_next_part(d, callback);
}

void tgl_download_manager::download_document(const std::shared_ptr<tgl_document>& document,
        const tgl_download_callback& callback)
{
    std::shared_ptr<download> d = std::make_shared<download>(CODE_input_document_file_location, document);
    download_document(document, d, callback);
}

void tgl_download_manager::download_video(const std::shared_ptr<tgl_document>& document,
        const tgl_download_callback& callback)
{
    std::shared_ptr<download> d = std::make_shared<download>(CODE_input_video_file_location, document);
    download_document(document, d, callback);
}

void tgl_download_manager::download_audio(const std::shared_ptr<tgl_document>& document,
        const tgl_download_callback& callback)
{
    std::shared_ptr<download> d = std::make_shared<download>(CODE_input_audio_file_location, document);
    download_document(document, d, callback);
}

void tgl_download_manager::download_encr_document(const std::shared_ptr<tgl_encr_document>& document,
        const tgl_download_callback& callback)
{
    assert(document);
    std::shared_ptr<download> d = std::make_shared<download>(document->size, document);
    d->key = document->key;
    d->iv = document->iv;
    if (!document->mime_type.empty()) {
        const char* extension  = tg_extension_by_mime(document->mime_type.c_str());
        if (extension) {
            d->ext = std::string(extension);
        }
    }
    download_next_part(d, callback);

    unsigned char md5[16];
    unsigned char str[64];
    memcpy(str, document->key.data(), 32);
    memcpy(str + 32, document->iv.data(), 32);
    TGLC_md5(str, 64, md5);
    assert(document->key_fingerprint == ((*(int *)md5) ^ (*(int *)(md5 + 4))));
}
