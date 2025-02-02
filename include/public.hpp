#pragma once

// 公共文件

enum EnMsgType
{
    LoginMsg = 1,       // 登录消息
    LoginMsgAck = 2,    // 登录消息的回应
    RegMsg = 3,         // 注册消息
    RegMsgAck = 4,      // 注册消息的回应
    OTOMsg = 5,         // 私聊消息
    AddFriendMsg = 6,   // 添加好友消息
    CreateGroupMsg = 7, // 创建群组消息
    AddGroupMsg = 8,    // 加入群组消息
    GroupChatMsg = 9,   // 群聊消息
    loginOutMsg = 10,   // 登出消息
};