#pragma once

// 图片数据访问层

#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/buffer.h>
#include "db.h"
#include "connectionpool.h"
#include <vector>
#include <cstring>
#include <opencv2/opencv.hpp>

class ImageModel
{
public:
    // 压缩图片（使用OpenCV）
    std::string compressImage(const std::string &image_data, int quality = 85, int max_size = 200)
    {
        try
        {
            std::vector<unsigned char> data(image_data.begin(), image_data.end());
            cv::Mat img = cv::imdecode(data, cv::IMREAD_COLOR);

            if (img.empty())
            {
                std::cout << "无法解码图片数据" << std::endl;
                return image_data;
            }

            // 调整图片大小
            if (img.cols > max_size || img.rows > max_size)
            {
                double scale = std::min(static_cast<double>(max_size) / img.cols,
                                        static_cast<double>(max_size) / img.rows);
                cv::resize(img, img, cv::Size(), scale, scale, cv::INTER_AREA);
            }

            // 压缩为JPEG格式
            std::vector<unsigned char> buffer;
            std::vector<int> params = {cv::IMWRITE_JPEG_QUALITY, quality};
            cv::imencode(".jpg", img, buffer, params);

            std::string result(buffer.begin(), buffer.end());

            std::cout << "原始图片大小: " << image_data.size() << " 字节" << std::endl;
            std::cout << "压缩后图片大小: " << result.size() << " 字节" << std::endl;

            return result;
        }
        catch (const cv::Exception &e)
        {
            std::cerr << "OpenCV异常: " << e.what() << std::endl;
            return image_data;
        }
        catch (const std::exception &e)
        {
            std::cerr << "标准异常: " << e.what() << std::endl;
            return image_data;
        }
        catch (...)
        {
            std::cerr << "未知异常" << std::endl;
            return image_data;
        }
    }

    // 插入图片
    void insert(const int id, const std::string &image_data)
    {
        std::string compressed_data = compressImage(image_data);

        ConnectionGuard guard;
        MySQL* mysql = guard.get();
        if (!mysql)
            return;

        MYSQL_STMT *stmt = mysql_stmt_init(mysql->get_conn());
        const char *sql = "INSERT INTO images(id, image_data) VALUES(?, ?)";
        mysql_stmt_prepare(stmt, sql, strlen(sql));

        MYSQL_BIND bind[2];
        memset(bind, 0, sizeof(bind));

        bind[0].buffer_type = MYSQL_TYPE_LONG;
        bind[0].buffer = (void *)&id;

        bind[1].buffer_type = MYSQL_TYPE_BLOB;
        bind[1].buffer = (void *)compressed_data.data();
        bind[1].buffer_length = compressed_data.size();

        mysql_stmt_bind_param(stmt, bind);
        mysql_stmt_execute(stmt);
        mysql_stmt_close(stmt);
    }

    // 查询图片
    string query(int image_id)
    {
        ConnectionGuard guard;
        MySQL* mysql = guard.get();
        if (!mysql)
            return "";

        MYSQL_STMT *stmt = mysql_stmt_init(mysql->get_conn());
        const char *sql = "SELECT image_data FROM images WHERE id=?";
        mysql_stmt_prepare(stmt, sql, strlen(sql));

        MYSQL_BIND param;
        memset(&param, 0, sizeof(param));
        param.buffer_type = MYSQL_TYPE_LONG;
        param.buffer = &image_id;
        mysql_stmt_bind_param(stmt, &param);

        unsigned long data_length;
        std::vector<char> buffer(10 * 1024 * 1024);
        MYSQL_BIND result;
        memset(&result, 0, sizeof(result));
        result.buffer_type = MYSQL_TYPE_BLOB;
        result.buffer = buffer.data();
        result.buffer_length = buffer.size();
        result.length = &data_length;

        mysql_stmt_bind_result(stmt, &result);

        if (mysql_stmt_execute(stmt) || mysql_stmt_fetch(stmt))
        {
            mysql_stmt_close(stmt);
            return "";
        }

        mysql_stmt_close(stmt);
        return base64_encode(buffer.data(), data_length);
    }

    // 更新图片
    bool update(int image_id, const string &new_image_data)
    {
        ConnectionGuard guard;
        MySQL* mysql = guard.get();
        if (!mysql)
            return false;

        MYSQL_STMT *stmt = mysql_stmt_init(mysql->get_conn());
        const char *sql = "UPDATE images SET image_data=? WHERE id=?";
        if (mysql_stmt_prepare(stmt, sql, strlen(sql)))
        {
            return false;
        }

        MYSQL_BIND bind[2];
        memset(bind, 0, sizeof(bind));

        bind[0].buffer_type = MYSQL_TYPE_BLOB;
        bind[0].buffer = (void *)new_image_data.data();
        bind[0].buffer_length = new_image_data.size();

        bind[1].buffer_type = MYSQL_TYPE_LONG;
        bind[1].buffer = &image_id;

        if (mysql_stmt_bind_param(stmt, bind))
        {
            mysql_stmt_close(stmt);
            return false;
        }

        bool ret = (mysql_stmt_execute(stmt) == 0);
        mysql_stmt_close(stmt);
        return ret;
    }

private:
    // Base64编码
    static string base64_encode(const char *data, size_t length)
    {
        BIO *bio, *b64;
        BUF_MEM *bufferPtr;

        b64 = BIO_new(BIO_f_base64());
        bio = BIO_new(BIO_s_mem());
        bio = BIO_push(b64, bio);
        BIO_set_flags(bio, BIO_FLAGS_BASE64_NO_NL);
        BIO_write(bio, data, length);
        BIO_flush(bio);
        BIO_get_mem_ptr(bio, &bufferPtr);

        string result(bufferPtr->data, bufferPtr->length);
        BIO_free_all(bio);
        return result;
    }
};