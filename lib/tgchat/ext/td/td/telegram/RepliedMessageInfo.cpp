//
// Copyright Aliaksei Levin (levlam@telegram.org), Arseny Smirnov (arseny30@gmail.com) 2014-2023
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
#include "td/telegram/RepliedMessageInfo.h"

#include "td/telegram/ContactsManager.h"
#include "td/telegram/Dependencies.h"
#include "td/telegram/MessageContent.h"
#include "td/telegram/MessageContentType.h"
#include "td/telegram/MessageCopyOptions.h"
#include "td/telegram/MessageFullId.h"
#include "td/telegram/MessagesManager.h"
#include "td/telegram/misc.h"
#include "td/telegram/OptionManager.h"
#include "td/telegram/ScheduledServerMessageId.h"
#include "td/telegram/ServerMessageId.h"
#include "td/telegram/Td.h"

#include "td/utils/algorithm.h"
#include "td/utils/logging.h"

namespace td {

static bool has_qts_messages(const Td *td, DialogId dialog_id) {
  switch (dialog_id.get_type()) {
    case DialogType::User:
    case DialogType::Chat:
      return td->option_manager_->get_option_integer("session_count") > 1;
    case DialogType::Channel:
    case DialogType::SecretChat:
      return false;
    case DialogType::None:
    default:
      UNREACHABLE();
      return false;
  }
}

RepliedMessageInfo::~RepliedMessageInfo() = default;

RepliedMessageInfo::RepliedMessageInfo(Td *td, tl_object_ptr<telegram_api::messageReplyHeader> &&reply_header,
                                       DialogId dialog_id, MessageId message_id, int32 date) {
  CHECK(reply_header != nullptr);
  if (reply_header->reply_to_scheduled_) {
    message_id_ = MessageId(ScheduledServerMessageId(reply_header->reply_to_msg_id_), date);
    if (message_id.is_valid_scheduled()) {
      if (reply_header->reply_to_peer_id_ != nullptr) {
        dialog_id_ = DialogId(reply_header->reply_to_peer_id_);
        LOG(ERROR) << "Receive reply to " << MessageFullId{dialog_id_, message_id_} << " in "
                   << MessageFullId{dialog_id, message_id};
        message_id_ = MessageId();
        dialog_id_ = DialogId();
      }
      if (message_id == message_id_) {
        LOG(ERROR) << "Receive reply to " << message_id_ << " in " << MessageFullId{dialog_id, message_id};
        message_id_ = MessageId();
      }
    } else {
      LOG(ERROR) << "Receive reply to " << message_id_ << " in " << MessageFullId{dialog_id, message_id};
      message_id_ = MessageId();
    }
    if (reply_header->reply_from_ != nullptr || reply_header->reply_media_ != nullptr) {
      LOG(ERROR) << "Receive reply from other chat " << to_string(reply_header) << " in "
                 << MessageFullId{dialog_id, message_id};
    }
  } else {
    if (reply_header->reply_to_msg_id_ != 0) {
      message_id_ = MessageId(ServerMessageId(reply_header->reply_to_msg_id_));
      if (reply_header->reply_to_peer_id_ != nullptr) {
        dialog_id_ = DialogId(reply_header->reply_to_peer_id_);
        if (!dialog_id_.is_valid()) {
          LOG(ERROR) << "Receive reply in invalid " << to_string(reply_header->reply_to_peer_id_);
          message_id_ = MessageId();
          dialog_id_ = DialogId();
        }
      }
      if (!message_id_.is_valid()) {
        LOG(ERROR) << "Receive " << to_string(reply_header) << " in " << MessageFullId{dialog_id, message_id};
        message_id_ = MessageId();
        dialog_id_ = DialogId();
      } else if (!message_id.is_scheduled() && !dialog_id_.is_valid() &&
                 ((message_id_ > message_id && !has_qts_messages(td, dialog_id)) || message_id_ == message_id)) {
        LOG(ERROR) << "Receive reply to " << message_id_ << " in " << MessageFullId{dialog_id, message_id};
        message_id_ = MessageId();
      }
    } else if (reply_header->reply_to_peer_id_ != nullptr) {
      LOG(ERROR) << "Receive " << to_string(reply_header) << " in " << MessageFullId{dialog_id, message_id};
    }
    if (reply_header->reply_from_ != nullptr) {
      origin_date_ = reply_header->reply_from_->date_;
      if (origin_date_ <= 0) {
        LOG(ERROR) << "Receive " << to_string(reply_header) << " in " << MessageFullId{dialog_id, message_id};
        origin_date_ = 0;
      } else {
        auto r_reply_origin = MessageOrigin::get_message_origin(td, std::move(reply_header->reply_from_));
        if (r_reply_origin.is_error()) {
          origin_date_ = 0;
        } else {
          origin_ = r_reply_origin.move_as_ok();
        }
      }
    }
    if (!origin_.is_empty() && reply_header->reply_media_ != nullptr &&
        reply_header->reply_media_->get_id() != telegram_api::messageMediaEmpty::ID) {
      content_ = get_message_content(td, FormattedText(), std::move(reply_header->reply_media_), dialog_id,
                                     origin_date_, true, UserId(), nullptr, nullptr, "messageReplyHeader");
      CHECK(content_ != nullptr);
      if (!is_supported_reply_message_content(content_->get_type())) {
        LOG(ERROR) << "Receive reply with media of the type " << content_->get_type();
        content_ = nullptr;
      }
    }
  }
  if ((!origin_.is_empty() || message_id_ != MessageId()) && !reply_header->quote_text_.empty()) {
    is_quote_manual_ = reply_header->quote_;
    auto entities = get_message_entities(td->contacts_manager_.get(), std::move(reply_header->quote_entities_),
                                         "RepliedMessageInfo");
    auto status = fix_formatted_text(reply_header->quote_text_, entities, true, true, true, true, false);
    if (status.is_error()) {
      if (!clean_input_string(reply_header->quote_text_)) {
        reply_header->quote_text_.clear();
      }
      entities.clear();
    }
    quote_ = FormattedText{std::move(reply_header->quote_text_), std::move(entities)};
    quote_position_ = max(0, reply_header->quote_offset_);
    remove_unallowed_quote_entities(quote_);
  }
}

RepliedMessageInfo::RepliedMessageInfo(Td *td, const MessageInputReplyTo &input_reply_to) {
  if (!input_reply_to.message_id_.is_valid()) {
    return;
  }
  message_id_ = input_reply_to.message_id_;
  if (!input_reply_to.quote_.text.empty()) {
    quote_ = input_reply_to.quote_;
    quote_position_ = input_reply_to.quote_position_;
    is_quote_manual_ = true;
  }
  if (input_reply_to.dialog_id_ != DialogId()) {
    auto info =
        td->messages_manager_->get_forwarded_message_info({input_reply_to.dialog_id_, input_reply_to.message_id_});
    if (info.origin_date_ == 0 || info.origin_.is_empty() || info.content_ == nullptr) {
      *this = {};
      return;
    }
    origin_date_ = info.origin_date_;
    origin_ = std::move(info.origin_);
    content_ = std::move(info.content_);
    auto content_text = get_message_content_text_mutable(content_.get());
    if (content_text != nullptr) {
      if (!is_quote_manual_) {
        quote_ = std::move(*content_text);
        remove_unallowed_quote_entities(quote_);
        truncate_formatted_text(
            quote_, static_cast<size_t>(td->option_manager_->get_option_integer("message_reply_quote_length_max")));
      }
      *content_text = {};
    }
    auto origin_message_full_id = origin_.get_message_full_id();
    if (origin_message_full_id.get_message_id().is_valid()) {
      message_id_ = origin_message_full_id.get_message_id();
      dialog_id_ = origin_message_full_id.get_dialog_id();
    } else if (input_reply_to.dialog_id_.get_type() == DialogType::Channel) {
      dialog_id_ = input_reply_to.dialog_id_;
    } else {
      message_id_ = MessageId();
    }
  }
}

RepliedMessageInfo RepliedMessageInfo::clone(Td *td) const {
  RepliedMessageInfo result;
  result.message_id_ = message_id_;
  result.dialog_id_ = dialog_id_;
  result.origin_date_ = origin_date_;
  result.origin_ = origin_;
  if (content_ != nullptr) {
    result.content_ = dup_message_content(td, DialogId(td->contacts_manager_->get_my_id()), content_.get(),
                                          MessageContentDupType::Forward, MessageCopyOptions());
  }
  result.quote_ = quote_;
  result.quote_position_ = quote_position_;
  result.is_quote_manual_ = is_quote_manual_;
  return result;
}

bool RepliedMessageInfo::need_reget() const {
  return content_ != nullptr && need_reget_message_content(content_.get());
}

bool RepliedMessageInfo::need_reply_changed_warning(
    const Td *td, const RepliedMessageInfo &old_info, const RepliedMessageInfo &new_info,
    MessageId old_top_thread_message_id, bool is_yet_unsent,
    std::function<bool(const RepliedMessageInfo &info)> is_reply_to_deleted_message) {
  if (old_info.origin_date_ != new_info.origin_date_ && old_info.origin_date_ != 0 && new_info.origin_date_ != 0) {
    // date of the original message can't change
    return true;
  }
  if (old_info.origin_ != new_info.origin_ && !old_info.origin_.has_sender_signature() &&
      !new_info.origin_.has_sender_signature() && !old_info.origin_.is_empty() && !new_info.origin_.is_empty()) {
    // only signature can change in the message origin
    return true;
  }
  if (old_info.quote_position_ != new_info.quote_position_ &&
      max(old_info.quote_position_, new_info.quote_position_) <
          static_cast<int32>(min(old_info.quote_.text.size(), new_info.quote_.text.size()))) {
    // quote position can't change
    return true;
  }
  if (old_info.is_quote_manual_ != new_info.is_quote_manual_) {
    // quote manual property can't change
    return true;
  }
  if (old_info.quote_ != new_info.quote_) {
    if (old_info.is_quote_manual_) {
      return true;
    }
    auto max_size = td->option_manager_->get_option_integer("message_reply_quote_length_max") - 70;
    if (static_cast<int64>(max(old_info.quote_.text.size(), new_info.quote_.text.size())) < max_size) {
      // automatic quote can't change, unless truncated differently
      return true;
    }
  }
  if (old_info.dialog_id_ != new_info.dialog_id_ && old_info.dialog_id_ != DialogId() &&
      new_info.dialog_id_ != DialogId()) {
    // reply chat can't change
    return true;
  }
  if (old_info.message_id_ == new_info.message_id_ && old_info.dialog_id_ == new_info.dialog_id_) {
    if (old_info.message_id_ != MessageId()) {
      if (old_info.origin_date_ != new_info.origin_date_) {
        // date of the original message can't change
        return true;
      }
      if (old_info.origin_ != new_info.origin_ && !old_info.origin_.has_sender_signature() &&
          !new_info.origin_.has_sender_signature()) {
        // only signature can change in the message origin
        return true;
      }
    }
    return false;
  }
  if (is_yet_unsent && is_reply_to_deleted_message(old_info) && new_info.message_id_ == MessageId()) {
    // reply to a deleted message, which was available locally
    return false;
  }
  if (is_yet_unsent && is_reply_to_deleted_message(new_info) && old_info.message_id_ == MessageId()) {
    // reply to a locally deleted yet unsent message, which was available server-side
    return false;
  }
  if (old_info.message_id_.is_valid_scheduled() && old_info.message_id_.is_scheduled_server() &&
      new_info.message_id_.is_valid_scheduled() && new_info.message_id_.is_scheduled_server() &&
      old_info.message_id_.get_scheduled_server_message_id() ==
          new_info.message_id_.get_scheduled_server_message_id()) {
    // schedule date change
    return false;
  }
  if (is_yet_unsent && old_top_thread_message_id == new_info.message_id_ && new_info.dialog_id_ == DialogId()) {
    // move of reply to the top thread message after deletion of the replied message
    return false;
  }
  return true;
}

vector<FileId> RepliedMessageInfo::get_file_ids(Td *td) const {
  if (content_ != nullptr) {
    return get_message_content_file_ids(content_.get(), td);
  }
  return {};
}

vector<UserId> RepliedMessageInfo::get_min_user_ids(Td *td) const {
  vector<UserId> user_ids;
  if (dialog_id_.get_type() == DialogType::User) {
    user_ids.push_back(dialog_id_.get_user_id());
  }
  origin_.add_user_ids(user_ids);
  // not supported server-side: add_formatted_text_user_ids(user_ids, &quote_);
  if (content_ != nullptr) {
    append(user_ids, get_message_content_min_user_ids(td, content_.get()));
  }
  return user_ids;
}

vector<ChannelId> RepliedMessageInfo::get_min_channel_ids(Td *td) const {
  vector<ChannelId> channel_ids;
  if (dialog_id_.get_type() == DialogType::Channel) {
    channel_ids.push_back(dialog_id_.get_channel_id());
  }
  origin_.add_channel_ids(channel_ids);
  if (content_ != nullptr) {
    append(channel_ids, get_message_content_min_channel_ids(td, content_.get()));
  }
  return channel_ids;
}

void RepliedMessageInfo::add_dependencies(Dependencies &dependencies, bool is_bot) const {
  dependencies.add_dialog_and_dependencies(dialog_id_);
  origin_.add_dependencies(dependencies);
  add_formatted_text_dependencies(dependencies, &quote_);
  if (content_ != nullptr) {
    add_message_content_dependencies(dependencies, content_.get(), is_bot);
  }
}

td_api::object_ptr<td_api::messageReplyToMessage> RepliedMessageInfo::get_message_reply_to_message_object(
    Td *td, DialogId dialog_id) const {
  if (dialog_id_.is_valid()) {
    dialog_id = dialog_id_;
  } else {
    CHECK(dialog_id.is_valid());
  }
  auto chat_id = td->messages_manager_->get_chat_id_object(dialog_id, "messageReplyToMessage");
  if (message_id_ == MessageId()) {
    chat_id = 0;
  }

  td_api::object_ptr<td_api::textQuote> quote;
  if (!quote_.text.empty()) {
    quote = td_api::make_object<td_api::textQuote>(get_formatted_text_object(quote_, true, -1), quote_position_,
                                                   is_quote_manual_);
  }

  td_api::object_ptr<td_api::MessageOrigin> origin;
  if (!origin_.is_empty()) {
    origin = origin_.get_message_origin_object(td);
    CHECK(origin != nullptr);
  }

  td_api::object_ptr<td_api::MessageContent> content;
  if (content_ != nullptr) {
    content = get_message_content_object(content_.get(), td, dialog_id, 0, false, true, -1, false, false);
    switch (content->get_id()) {
      case td_api::messageUnsupported::ID:
        content = nullptr;
        break;
      case td_api::messageText::ID: {
        const auto *message_text = static_cast<const td_api::messageText *>(content.get());
        if (message_text->web_page_ == nullptr && message_text->link_preview_options_ == nullptr) {
          content = nullptr;
        }
        break;
      }
      default:
        // nothing to do
        break;
    }
  }

  return td_api::make_object<td_api::messageReplyToMessage>(chat_id, message_id_.get(), std::move(quote),
                                                            std::move(origin), origin_date_, std::move(content));
}

MessageInputReplyTo RepliedMessageInfo::get_input_reply_to() const {
  CHECK(!is_external());
  if (message_id_.is_valid()) {
    return MessageInputReplyTo{message_id_, dialog_id_, FormattedText{quote_}, quote_position_};
  }
  return {};
}

MessageId RepliedMessageInfo::get_same_chat_reply_to_message_id(bool ignore_external) const {
  if (message_id_ == MessageId()) {
    return {};
  }
  if (ignore_external && !origin_.is_empty()) {
    return {};
  }
  return dialog_id_ == DialogId() ? message_id_ : MessageId();
}

MessageFullId RepliedMessageInfo::get_reply_message_full_id(DialogId owner_dialog_id, bool ignore_external) const {
  if (message_id_ == MessageId()) {
    return {};
  }
  if (ignore_external && !origin_.is_empty()) {
    return {};
  }
  return {dialog_id_.is_valid() ? dialog_id_ : owner_dialog_id, message_id_};
}

void RepliedMessageInfo::register_content(Td *td) const {
  if (content_ != nullptr) {
    register_reply_message_content(td, content_.get());
  }
}

void RepliedMessageInfo::unregister_content(Td *td) const {
  if (content_ != nullptr) {
    unregister_reply_message_content(td, content_.get());
  }
}

bool operator==(const RepliedMessageInfo &lhs, const RepliedMessageInfo &rhs) {
  if (!(lhs.message_id_ == rhs.message_id_ && lhs.dialog_id_ == rhs.dialog_id_ &&
        lhs.origin_date_ == rhs.origin_date_ && lhs.origin_ == rhs.origin_ && lhs.quote_ == rhs.quote_ &&
        lhs.quote_position_ == rhs.quote_position_ && lhs.is_quote_manual_ == rhs.is_quote_manual_)) {
    return false;
  }
  bool need_update = false;
  bool is_content_changed = false;
  compare_message_contents(nullptr, lhs.content_.get(), rhs.content_.get(), is_content_changed, need_update);
  if (need_update || is_content_changed) {
    return false;
  }
  return true;
}

bool operator!=(const RepliedMessageInfo &lhs, const RepliedMessageInfo &rhs) {
  return !(lhs == rhs);
}

StringBuilder &operator<<(StringBuilder &string_builder, const RepliedMessageInfo &info) {
  string_builder << "reply to " << info.message_id_;
  if (info.dialog_id_ != DialogId()) {
    string_builder << " in " << info.dialog_id_;
  }
  if (info.origin_date_ != 0) {
    string_builder << " sent at " << info.origin_date_ << " by " << info.origin_;
  }
  if (!info.quote_.text.empty()) {
    string_builder << " with " << info.quote_.text.size() << (info.is_quote_manual_ ? " manually" : "")
                   << " quoted bytes";
    if (info.quote_position_ != 0) {
      string_builder << " at position " << info.quote_position_;
    }
  }
  if (info.content_ != nullptr) {
    string_builder << " and content of the type " << info.content_->get_type();
  }
  return string_builder;
}

}  // namespace td
