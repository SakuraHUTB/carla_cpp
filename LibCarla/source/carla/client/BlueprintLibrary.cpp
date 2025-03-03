﻿// Copyright (c) 2017 Computer Vision Center (CVC) at the Universitat Autonoma
// de Barcelona (UAB).
//
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT>.

#include "carla/client/BlueprintLibrary.h" // 引入Carla客户端库中的BlueprintLibrary头文件，该文件包含了与Carla模拟器交互所需的蓝图相关功能。

#include "carla/Exception.h" // 引入Carla异常处理相关的头文件，这个头文件包含了Carla模拟器中可能抛出的异常类，方便进行错误处理。

#include <algorithm> // 引入标准库中的算法功能，该头文件包含了各种常见的算法，如排序、查找等，可以在容器中使用。
#include <iterator> // 引入标准库中的迭代器相关功能，该头文件定义了用于遍历容器的迭代器功能，例如 std::begin 和 std::end。

namespace carla {
namespace client {
//构造函数：使用给定的蓝图列表初始化 BlueprintLibary
  BlueprintLibrary::BlueprintLibrary(
      const std::vector<rpc::ActorDefinition> &blueprints) {
    _blueprints.reserve(blueprints.size()); //为存储蓝图预留空间
    for (auto &definition : blueprints) {
      _blueprints.emplace(definition.id, ActorBlueprint{definition});
      //将每个蓝图按其 ID 添加到映射中
    }
  }
//根据通配符模式过滤蓝图，返回匹配的 BlueprintLibrary 对象
  SharedPtr<BlueprintLibrary> BlueprintLibrary::Filter(
      const std::string &wildcard_pattern) const {
    map_type result; //用于存储过滤后的蓝图映射
    for (auto &pair : _blueprints) {
      if (pair.second.MatchTags(wildcard_pattern)) { //检查蓝图是否匹配通配符模式
        result.emplace(pair); // 如果蓝图匹配通配符模式，则添加到结果中
      }
    }
    return SharedPtr<BlueprintLibrary>{new BlueprintLibrary(result)};
    // 返回过滤后的 BlueprintLibrary 对象
  }
//根据属性名称和值过滤蓝图，返回匹配的 BlueprintLibrary 对象
  SharedPtr<BlueprintLibrary> BlueprintLibrary::FilterByAttribute(
      const std::string &name, const std::string& value) const {
    map_type result; //用于存储过滤后的蓝图映射

    for (auto &pair : _blueprints) {
      if (!pair.second.ContainsAttribute(name))
        continue; //如果蓝图不包含指定的属性，则跳过
      const ActorAttribute &Attribute = pair.second.GetAttribute(name); //获取指定的属性
      const std::vector<std::string> &Values = Attribute.GetRecommendedValues();//获取推荐值
      if (Values.empty()) // 如果没有推荐值，检查当前属性值
      {
        const std::string &AttributeValue = Attribute.GetValue(); //获取属性当前值
        if (value == AttributeValue) 
          result.emplace(pair); // 如果属性值匹配，则添加到结果中
      }
      else
      {
        for (const std::string &Value : Values) // 遍历推荐值列表
        {
          if (Value == value) 
          {
            result.emplace(pair); // 如果找到匹配值，添加到结果中
            break; //找到匹配后退出循环
          }
        }
      }

    }
    return SharedPtr<BlueprintLibrary>{new BlueprintLibrary(result)};  // 返回过滤后的 BlueprintLibrary 对象
  }

  // 查找并返回与给定键匹配的蓝图，如果未找到返回 nullptr
  BlueprintLibrary::const_pointer BlueprintLibrary::Find(const std::string &key) const {
    auto it = _blueprints.find(key); // 在 _blueprints 字典中查找给定的键
    return it != _blueprints.end() ? &it->second : nullptr;
    // 如果找到对应键，返回其值的指针，否则返回 nullptr
  }

  // 根据指定的键返回蓝图的常量引用，如果未找到键则抛出异常
  BlueprintLibrary::const_reference BlueprintLibrary::at(const std::string &key) const {
    auto it = _blueprints.find(key); // 在 _blueprints 字典中查找给定的键
    if (it == _blueprints.end()) {
      using namespace std::string_literals;
      throw_exception(std::out_of_range("blueprint '"s + key + "' not found"));
    } //如果未找到对应的键，抛出 std::out_of_range 异常
    return it->second;//如果找到对应的键，则返回对应键的蓝图的常量引用
  }

  // 根据指定位置返回蓝图的常量引用，如果位置超出范围则抛出异常
  BlueprintLibrary::const_reference BlueprintLibrary::at(size_type pos) const {
  	//检查 pos 是否超出 _blueprints 的大小范围
    if (pos >= size()) {
      throw_exception(std::out_of_range("index out of range"));
    } //如果位置超出范围，抛出 std::out_of_range 异常
    return operator[](pos); //返回指定位置的蓝图常量引用
  }

} // namespace client
} // namespace carla
