#include "server/Server.hpp"

#include <iostream>

#include "common/FunctionCall.hpp"

bool Server::connectToBinder() {
  char* binderHostname = getenv("BINDER_ADDRESS");
  if (!binderHostname) {
    std::cerr << "Could not get BINDER_ADDRESS from environment!" << std::endl;
    return false;
  }
  char* portStr = getenv("BINDER_PORT");
  if (!portStr) {
    std::cerr << "Could not get BINDER_PORT from environment!" << std::endl;
    return false;
  }

  // Binder information
  int binderPort = std::stoi(portStr);
  binderServer = gethostbyname(binderHostname);
  binderAddr.sin_family = AF_INET;
  std::memcpy((char*)&binderAddr.sin_addr.s_addr,
              (char*)binderServer->h_addr,
              (int)binderServer->h_length);
  binderAddr.sin_port = htons(binderPort);

  // Now connect to binder socket
  binderSocket = socket(AF_INET, SOCK_STREAM, 0);
  if (binderSocket < 0) {
    return false;
  }
  if (::connect(
        binderSocket, (struct sockaddr*)&binderAddr, sizeof(binderAddr)) < 0) {
    return false;
  }
  binderClientList.clients.emplace_back(binderSocket);
  binderConnection = &binderClientList.clients.front();
  int success = binderConnection->send(
      Message::Type::SERVER_REGISTRATION,
      serverAddress.serialize());
  if (success < 0) {
    std::cerr<<"registration send not success"<<std::endl;
    return false;
  }
  Message message;
  if (binderConnection->read(&message) < 0) {
    // Error in reading
    std::cerr << "error reading registeration"<<std::endl;
    return false;
  }
  if (message.type != Message::Type::SERVER_REGISTRATION) {
    // Error from binder
    binderConnection->close();
    std::cerr << "Bad msg type from registration"<<std::endl;
    return false;
  }
  return true;
}

bool Server::connect() {
  return server.connect(&serverAddress) && connectToBinder();
}

bool Server::run() {
  if (registeredFunctions.size() == 0) {
    // Did not register any functions: Error
    return false;
  }

  server.addClientList(&clients);
  server.addClientList(&binderClientList);
  if (binderConnection->send(Message::Type::SERVER_READY, "") < 0) {
    return false;
  }
  Message message;
  if (binderConnection->read(&message) < 0 ||
      message.type != Message::Type::SERVER_READY) {
    binderConnection->close();
    return false;
  }

  // TODO: Join message handling threads
  return server.serve();
}

int Server::registerRpc(const std::string& name, int* argTypes, skeleton f) {
  bool warning = false;
  FunctionSignature signature(name, argTypes);
  // Used if we have the same signature as an existing function (overwrite)
  ServerFunction* oldFunction = nullptr;
  for (auto& function : registeredFunctions) {
    if (function.signature == signature) {
      warning = true;
      oldFunction = &function;
      break;
    }
  }

  // Now send to binder
  if (binderConnection->send(
      Message::Type::RPC_REGISTRATION, signature.serialize()) < 0) {
    // Error
    std::cerr << "rpc reg Send not success" << std::endl;
    return -1;
  }
  Message message;
  if (binderConnection->read(&message) < 0) {
    std::cerr << "rpc reg read fail" << std::endl;
    return -1;
  }
  if (message.type != Message::Type::RPC_REGISTRATION) {
    std::cerr << "rpc reg msg type bad " << std::endl;
    return -1;
  }

  if (oldFunction != nullptr) {
    *oldFunction = ServerFunction(signature, f);
  }
  else {
    registeredFunctions.emplace_back(signature, f);
  }

  return warning ? 1 : 0;
}

bool Server::handleMessage(const Message& message, Connection& conn) {
  switch (message.type) {
  case Message::Type::CALL:
    handleCall(message, conn);
    break;

  default:
    // Some invalid type
    conn.close();
    break;
  }
  // Always remove it from the list
  return true;
}

bool Server::handleBinderMessage(const Message& message, Connection& conn) {
  if (message.type == Message::Type::TERMINATION) {
    // terminate
    // TODO : May need thread cleanup, etc
    conn.close();
    server.stop();
    return true;
  }
  else {
    conn.close();
    server.stop();
    return false;
  }
}

Server::ServerFunction* Server::getFunction(
    const FunctionSignature& signature) {
  for (auto& function : registeredFunctions) {
    if (function.signature == signature) {
      return &function;
    }
  }
  return nullptr;
}

void Server::handleCall(const Message& message, Connection& conn) {
  // Call me maybe...
  auto functionCall = FunctionCall::deserialize(message);
  ServerFunction* function = getFunction(functionCall.signature);
  if (function == nullptr) {
    conn.close();
    return;
  }
  int* argTypes = functionCall.signature.getArgTypes();
  void** args = functionCall.getArgArray();
  int ret = function->function(argTypes, args);
  if (ret < 0) {
    // Error lol
    conn.close();
    return;
  }
  functionCall = FunctionCall(std::move(functionCall.signature), args);
  conn.send(Message::Type::CALL, functionCall.serialize());
}
