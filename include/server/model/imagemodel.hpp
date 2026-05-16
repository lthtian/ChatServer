#pragma once

// 图片数据访问层

#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/buffer.h>
#include "async_connectionpool.hpp"
#include <boost/asio.hpp>
#include <boost/mysql.hpp>
#include <vector>
#include <cstring>
#include <opencv2/opencv.hpp>

namespace asio = boost::asio;
namespace mysql = boost::mysql;

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
    asio::awaitable<void> insert(const int id, const std::string &image_data)
    {
        std::string compressed_data = compressImage(image_data);

        auto guard = co_await AsyncConnectionGuard::create();
        if (!guard->valid())
            co_return;

        auto& conn = guard->connection();

        auto stmt = co_await conn.async_prepare_statement(
            "INSERT INTO images(id, image_data) VALUES(?, ?)",
            asio::use_awaitable);

        // boost::mysql blob 参数：使用 string 作为 blob_view
        mysql::results result;
        co_await conn.async_execute(
            stmt.bind(id, mysql::blob_view(reinterpret_cast<const unsigned char*>(compressed_data.data()), compressed_data.size())),
            result, asio::use_awaitable);
    }

    // 查询图片
    asio::awaitable<string> query(int image_id)
    {
        auto guard = co_await AsyncConnectionGuard::create();
        if (!guard->valid())
            co_return "";

        auto& conn = guard->connection();

        auto stmt = co_await conn.async_prepare_statement(
            "SELECT image_data FROM images WHERE id=?",
            asio::use_awaitable);

        mysql::results result;
        co_await conn.async_execute(stmt.bind(image_id), result, asio::use_awaitable);

        auto rows = result.rows();
        if (!rows.empty())
        {
            auto blob_data = rows[0][0].as_blob();
            co_return base64_encode(
                reinterpret_cast<const char*>(blob_data.data()),
                blob_data.size());
        }
        co_return "";
    }

    // 更新图片
    asio::awaitable<bool> update(int image_id, const string &new_image_data)
    {
        auto guard = co_await AsyncConnectionGuard::create();
        if (!guard->valid())
            co_return false;

        auto& conn = guard->connection();

        auto stmt = co_await conn.async_prepare_statement(
            "UPDATE images SET image_data=? WHERE id=?",
            asio::use_awaitable);

        mysql::results result;
        co_await conn.async_execute(
            stmt.bind(
                mysql::blob_view(reinterpret_cast<const unsigned char*>(new_image_data.data()), new_image_data.size()),
                image_id),
            result, asio::use_awaitable);

        co_return result.affected_rows() > 0;
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
