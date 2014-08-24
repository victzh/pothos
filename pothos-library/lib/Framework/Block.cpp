// Copyright (c) 2014-2014 Josh Blum
// SPDX-License-Identifier: BSL-1.0

#include "Framework/WorkerActor.hpp"
#include <Pothos/Object/Containers.hpp>

/***********************************************************************
 * Reusable threadpool
 **********************************************************************/
static std::shared_ptr<Theron::Framework> getGlobalFramework(void)
{
    static std::weak_ptr<Theron::Framework> weakFramework;
    std::shared_ptr<Theron::Framework> framework = weakFramework.lock();
    if (not framework) framework.reset(new Theron::Framework());
    weakFramework = framework;
    return framework;
}

void Pothos::Block::setThreadPool(const ThreadPool &threadPool)
{
    if (_threadPool == threadPool) return; //no change

    auto oldThreadPool = _threadPool;
    auto oldActor = _actor;

    _threadPool = threadPool;
    _actor.reset(new WorkerActor(this));
    _actor->swap(oldActor.get());

    oldActor.reset();
}

const Pothos::ThreadPool &Pothos::Block::getThreadPool(void) const
{
    return _threadPool;
}

/***********************************************************************
 * Block member implementation
 **********************************************************************/
Pothos::Block::Block(void):
    _threadPool(ThreadPool(getGlobalFramework())),
    _actor(new WorkerActor(this))
{
    return;
}

Pothos::Block::~Block(void)
{
    //Send a shutdown message and wait for response:
    //This allows the actor to finish with messages ahead of the shutdown message.
    Theron::Receiver receiver;
    ShutdownActorMessage message;
    _actor->GetFramework().Send(message, receiver.GetAddress(), _actor->GetAddress());
    receiver.Wait();
    _actor.reset();
}

void Pothos::Block::work(void)
{
    return;
}

void Pothos::Block::activate(void)
{
    return;
}

void Pothos::Block::deactivate(void)
{
    return;
}

void Pothos::Block::propagateLabels(const InputPort *input)
{
    for (auto &entry : this->allOutputs())
    {
        auto &output = entry.second;
        for (const auto &label : input->labels())
        {
            output->postLabel(label);
        }
    }
}

Pothos::WorkStats Pothos::Block::workStats(void) const
{
    InfoReceiver<WorkStats> receiver;
    _actor->GetFramework().Send(RequestWorkerStatsMessage(), receiver.GetAddress(), _actor->GetAddress());
    return receiver.WaitInfo();
}

bool Pothos::Block::isActive(void) const
{
    return _actor->activeState;
}

void Pothos::Block::setupInput(const std::string &name, const DType &dtype, const std::string &domain)
{
    _actor->allocateInput(name, dtype, domain);
}

void Pothos::Block::setupInput(const size_t index, const DType &dtype, const std::string &domain)
{
    this->setupInput(std::to_string(index), dtype, domain);
}

void Pothos::Block::setupOutput(const std::string &name, const DType &dtype, const std::string &domain)
{
    _actor->allocateOutput(name, dtype, domain);
}

void Pothos::Block::setupOutput(const size_t index, const DType &dtype, const std::string &domain)
{
    this->setupOutput(std::to_string(index), dtype, domain);
}

void Pothos::Block::registerCallable(const std::string &name, const Callable &call)
{
    _calls[name] = call;
    if (call.getNumArgs() > 0) this->registerSlot(name);
}

void Pothos::Block::registerSignal(const std::string &name)
{
    _actor->allocateSignal(name);
}

void Pothos::Block::registerSlot(const std::string &name)
{
    _actor->allocateSlot(name);
}

Pothos::Object Pothos::Block::opaqueCallHandler(const std::string &name, const Pothos::Object *inputArgs, const size_t numArgs)
{
    auto it = _calls.find(name);
    if (it == _calls.end())
    {
        throw Pothos::BlockCallNotFound("Pothos::Block::call("+name+")", "method does not exist in registry");
    }
    return it->second.opaqueCall(inputArgs, numArgs);
}

Pothos::Object Pothos::Block::opaqueCallMethod(const std::string &name, const Pothos::Object *inputArgs, const size_t numArgs) const
{
    //call into a signal
    auto out = _actor->outputs.find(name);
    if (out != _actor->outputs.end() and out->second->isSignal())
    {
        const ObjectVector args(inputArgs, inputArgs+numArgs);
        out->second->postMessage(Object(args));
        return Object();
    }

    //otherwise its a regular method
    auto it = _calls.find(name);
    if (it == _calls.end())
    {
        throw Pothos::BlockCallNotFound("Pothos::Block::call("+name+")", "method does not exist in registry");
    }

    OpaqueCallMessage message;
    message.name = name;
    message.inputArgs = inputArgs;
    message.numArgs = numArgs;

    InfoReceiver<OpaqueCallResultMessage> receiver;
    _actor->GetFramework().Send(message, receiver.GetAddress(), _actor->GetAddress());

    OpaqueCallResultMessage result = receiver.WaitInfo();
    if (result.error) result.error->rethrow();
    return result.obj;
}

void Pothos::Block::yield(void)
{
    _actor->workBump = true;
}

std::shared_ptr<Pothos::BufferManager> Pothos::Block::getInputBufferManager(const std::string &, const std::string &)
{
    return Pothos::BufferManager::Sptr(); //abdicate
}

std::shared_ptr<Pothos::BufferManager> Pothos::Block::getOutputBufferManager(const std::string &, const std::string &)
{
    return Pothos::BufferManager::Sptr(); //abdicate
}

std::vector<Pothos::PortInfo> Pothos::Block::inputPortInfo(void)
{
    std::vector<PortInfo> infos;
    for (const auto &name : _inputPortNames)
    {
        PortInfo info;
        info.name = name;
        info.isSigSlot = this->input(name)->isSlot();
        info.dtype = this->input(name)->dtype();
        infos.push_back(info);
    }
    return infos;
}

std::vector<Pothos::PortInfo> Pothos::Block::outputPortInfo(void)
{
    std::vector<PortInfo> infos;
    for (const auto &name : _outputPortNames)
    {
        PortInfo info;
        info.name = name;
        info.isSigSlot = this->output(name)->isSignal();
        info.dtype = this->output(name)->dtype();
        infos.push_back(info);
    }
    return infos;
}

#include <Pothos/Managed.hpp>

static Pothos::Block *getPointer(Pothos::Block &b)
{
    return &b;
}

static auto managedBlock = Pothos::ManagedClass()
    .registerClass<Pothos::Block>()
    .registerBaseClass<Pothos::Block, Pothos::Connectable>()
    .registerMethod("getPointer", &getPointer)
    .registerField(POTHOS_FCN_TUPLE(Pothos::Block, _actor))
    .registerMethod(POTHOS_FCN_TUPLE(Pothos::Block, workInfo))
    .registerMethod(POTHOS_FCN_TUPLE(Pothos::Block, workStats))

    //all of the setups with default args set
    .registerMethod(POTHOS_FCN_TUPLE(Pothos::Block, setThreadPool))
    .registerMethod(POTHOS_FCN_TUPLE(Pothos::Block, getThreadPool))
    .registerMethod<const std::string &>(POTHOS_FCN_TUPLE(Pothos::Block, setupInput))
    .registerMethod<const size_t>(POTHOS_FCN_TUPLE(Pothos::Block, setupInput))
    .registerMethod<const std::string &>(POTHOS_FCN_TUPLE(Pothos::Block, setupOutput))
    .registerMethod<const size_t>(POTHOS_FCN_TUPLE(Pothos::Block, setupOutput))

    .registerMethod("setupInput", Pothos::Callable::make<const std::string &>(&Pothos::Block::setupInput).bind("", 3))
    .registerMethod("setupInput", Pothos::Callable::make<const std::string &>(&Pothos::Block::setupInput).bind("", 3).bind("byte", 2))
    .registerMethod("setupInput", Pothos::Callable::make<const size_t>(&Pothos::Block::setupInput).bind("", 3))
    .registerMethod("setupInput", Pothos::Callable::make<const size_t>(&Pothos::Block::setupInput).bind("", 3).bind("byte", 2))

    .registerMethod("setupOutput", Pothos::Callable::make<const std::string &>(&Pothos::Block::setupOutput).bind("", 3))
    .registerMethod("setupOutput", Pothos::Callable::make<const std::string &>(&Pothos::Block::setupOutput).bind("", 3).bind("byte", 2))
    .registerMethod("setupOutput", Pothos::Callable::make<const size_t>(&Pothos::Block::setupOutput).bind("", 3))
    .registerMethod("setupOutput", Pothos::Callable::make<const size_t>(&Pothos::Block::setupOutput).bind("", 3).bind("byte", 2))

    .registerMethod(POTHOS_FCN_TUPLE(Pothos::Block, registerSignal))
    .registerMethod(POTHOS_FCN_TUPLE(Pothos::Block, registerSlot))
    .registerMethod(POTHOS_FCN_TUPLE(Pothos::Block, inputs))
    .registerMethod(POTHOS_FCN_TUPLE(Pothos::Block, allInputs))
    .registerMethod<const std::string &>(POTHOS_FCN_TUPLE(Pothos::Block, input))
    .registerMethod<const size_t>(POTHOS_FCN_TUPLE(Pothos::Block, input))
    .registerMethod(POTHOS_FCN_TUPLE(Pothos::Block, outputs))
    .registerMethod(POTHOS_FCN_TUPLE(Pothos::Block, allOutputs))
    .registerMethod<const std::string &>(POTHOS_FCN_TUPLE(Pothos::Block, output))
    .registerMethod<const size_t>(POTHOS_FCN_TUPLE(Pothos::Block, output))
    .registerMethod(POTHOS_FCN_TUPLE(Pothos::Block, yield))
    .commit("Pothos/Block");

template <typename PortType>
size_t portVectorSize(const std::vector<PortType*> &ports)
{
    return ports.size();
}

template <typename PortType>
PortType *portVectorAt(const std::vector<PortType*> &ports, const size_t index)
{
    return ports.at(index);
}

template <typename PortType>
size_t portMapSize(const std::map<std::string, PortType*> &ports)
{
    return ports.size();
}

template <typename PortType>
PortType *portMapAt(const std::map<std::string, PortType*> &ports, const std::string &key)
{
    return ports.at(key);
}

template <typename PortType>
std::vector<std::string> portMapKeys(const std::map<std::string, PortType*> &ports)
{
    std::vector<std::string> keys;
    for (const auto &pair : ports) keys.push_back(pair.first);
    return keys;
}

static auto managedInputPortVector = Pothos::ManagedClass()
    .registerClass<std::vector<Pothos::InputPort*>>()
    .registerMethod("size", &portVectorSize<Pothos::InputPort>)
    .registerMethod("at", &portVectorAt<Pothos::InputPort>)
    .commit("Pothos/InputPortVector");

static auto managedInputPortMap = Pothos::ManagedClass()
    .registerClass<std::map<std::string, Pothos::InputPort*>>()
    .registerMethod("size", &portMapSize<Pothos::InputPort>)
    .registerMethod("at", &portMapAt<Pothos::InputPort>)
    .registerMethod("keys", &portMapKeys<Pothos::InputPort>)
    .commit("Pothos/InputPortMap");

static auto managedOutputPortVector = Pothos::ManagedClass()
    .registerClass<std::vector<Pothos::OutputPort*>>()
    .registerMethod("size", &portVectorSize<Pothos::OutputPort>)
    .registerMethod("at", &portVectorAt<Pothos::OutputPort>)
    .commit("Pothos/OutputPortVector");

static auto managedOutputPortMap = Pothos::ManagedClass()
    .registerClass<std::map<std::string, Pothos::OutputPort*>>()
    .registerMethod("size", &portMapSize<Pothos::OutputPort>)
    .registerMethod("at", &portMapAt<Pothos::OutputPort>)
    .registerMethod("keys", &portMapKeys<Pothos::OutputPort>)
    .commit("Pothos/OutputPortMap");
