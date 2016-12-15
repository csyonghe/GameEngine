// Compiler.cpp : Defines the entry point for the console application.
//
#include "../CoreLib/Basic.h"
#include "../CoreLib/LibIO.h"
#include "ShaderCompiler.h"
#include "Lexer.h"
#include "Parser.h"
#include "Preprocessor.h"
#include "SyntaxVisitors.h"
#include "StdInclude.h"
#include "Schedule.h"
#include "CodeGenBackend.h"
#include "../CoreLib/Tokenizer.h"
#include "Closure.h"
#include "VariantIR.h"

#ifdef CreateDirectory
#undef CreateDirectory
#endif

using namespace CoreLib::Basic;
using namespace CoreLib::IO;
using namespace Spire::Compiler;

namespace Spire
{
	namespace Compiler
	{
		int compilerInstances = 0;

		class ShaderCompilerImpl : public ShaderCompiler
		{
		private:
			Dictionary<String, RefPtr<CodeGenBackend>> backends;

			void ResolveAttributes(SymbolTable * symTable)
			{
				for (auto & shader : symTable->ShaderDependenceOrder)
				{
					auto comps = shader->GetComponentDependencyOrder();
					for (auto & comp : comps)
					{
						for (auto & impl : comp->Implementations)
							for (auto & attrib : impl->SyntaxNode->LayoutAttributes)
							{
								try
								{
									if (attrib.Value.StartsWith("%"))
									{
										CoreLib::Text::TokenReader parser(attrib.Value.SubString(1, attrib.Value.Length() - 1));
										auto compName = parser.ReadWord();
										parser.Read(".");
										auto compAttrib = parser.ReadWord();
										RefPtr<ShaderComponentSymbol> compSym;
										if (shader->Components.TryGetValue(compName, compSym))
										{
											for (auto & timpl : compSym->Implementations)
											{
												String attribValue;
												if (timpl->SyntaxNode->LayoutAttributes.TryGetValue(compAttrib, attribValue))
													attrib.Value = attribValue;
											}
										}
									}
								}
								catch (Exception)
								{
								}
							}
					}
				}
			}

			/* Generate a shader variant by applying mechanic choice rules and the choice file.
			   The choice file provides "preferred" definitions, as represented in ShaderComponentSymbol::Type::PinnedWorlds
		       The process resolves the component references by picking a pinned definition if one is available, or a definition
			   with the preferred import path as defined by import operator ordering.
			   After all references are resolved, all unreferenced definitions (dead code) are eliminated, 
			   resulting a shader variant ready for code generation.
			*/
			RefPtr<ShaderIR> GenerateShaderVariantIR(CompileResult & cresult, ShaderClosure * shader, Schedule & schedule, SymbolTable * symbolTable)
			{
				RefPtr<ShaderIR> result = new ShaderIR();
				result->Shader = shader;
				result->SymbolTable = symbolTable;
				// mark pinned worlds
				for (auto & comp : shader->Components)
				{
					for (auto & impl : comp.Value->Implementations)
					{
						for (auto & w : impl->Worlds)
						{
							if (impl->SrcPinnedWorlds.Contains(w) || impl->SyntaxNode->IsInline || impl->ExportWorlds.Contains(w) || impl->SyntaxNode->IsInput)
							{
								comp.Value->Type->PinnedWorlds.Add(w);
							}
						}
					}
				}
				// apply choices
				Dictionary<String, ShaderComponentSymbol*> choiceComps;
				for (auto & comp : shader->AllComponents)
				{
					for (auto & choiceName : comp.Value->ChoiceNames)
						choiceComps[choiceName] = comp.Value;
				}
				HashSet<ShaderComponentImplSymbol*> pinnedImpl;
				for (auto & choice : schedule.Choices)
				{
					ShaderComponentSymbol * comp = nullptr;
					if (choiceComps.TryGetValue(choice.Key, comp))
					{
						comp->Type->PinnedWorlds.Clear();
						for (auto & selectedDef : choice.Value)
						{
							if (comp->Type->ConstrainedWorlds.Contains(selectedDef->WorldName))
							{
								comp->Type->PinnedWorlds.Add(selectedDef->WorldName);
								// find specified impl
								for (auto & impl : comp->Implementations)
								{
									if (impl->AlternateName == selectedDef->AlternateName && impl->Worlds.Contains(selectedDef->WorldName))
										pinnedImpl.Add(impl.Ptr());
								}
							}
							else
							{
                                cresult.GetErrorWriter()->diagnose(selectedDef.Ptr()->Position, Diagnostics::worldIsNotAValidChoiceForKey, selectedDef->WorldName, choice.Key);
							}
						}
					}
				}
				for (auto & attribs : schedule.AddtionalAttributes)
				{
					ShaderComponentSymbol * comp = nullptr;
					if (choiceComps.TryGetValue(attribs.Key, comp))
					{
						// apply attributes
						for (auto & impl : comp->Implementations)
						{
							for (auto & attrib : attribs.Value)
								impl->SyntaxNode->LayoutAttributes[attrib.Key] = attrib.Value;
						}
					}
				}
				// generate definitions
				for (auto & comp : shader->AllComponents)
				{
					EnumerableDictionary<String, ComponentDefinitionIR*> defs;
					Dictionary<String, ShaderComponentImplSymbol*> impls;
					for (auto & impl : comp.Value->Implementations)
					{
						for (auto & w : impl->Worlds)
						{
							RefPtr<ComponentDefinitionIR> def = new ComponentDefinitionIR();
							def->OriginalName = comp.Value->Name;
							def->UniqueKey = comp.Value->UniqueKey;
							def->UniqueName = comp.Value->UniqueName;
							def->Type = comp.Value->Type->DataType;
							def->IsEntryPoint = (impl->ExportWorlds.Contains(w) ||
								(shader->Pipeline->IsAbstractWorld(w) &&
								(impl->SyntaxNode->LayoutAttributes.ContainsKey("Pinned") || shader->Pipeline->Worlds[w]().SyntaxNode->LayoutAttributes.ContainsKey("Pinned"))));
							CloneContext cloneCtx;
							def->SyntaxNode = impl->SyntaxNode->Clone(cloneCtx);
							def->World = w;
							result->Definitions.Add(def);
							bool existingDefIsPinned = false;
							if (defs.ContainsKey(w))
								existingDefIsPinned = pinnedImpl.Contains(impls[w]());
							if (!existingDefIsPinned)
							{
								defs[w] = def.Ptr();
								impls[w] = impl.Ptr();
							}
						}
					}
					result->DefinitionsByComponent[comp.Key] = defs;
				}
				bool changed = true;
				while (changed)
				{
					changed = false;
					result->ResolveComponentReference();
					result->EliminateDeadCode();
					// check circular references
					for (auto & def : result->Definitions)
					{
						if (def->Dependency.Contains(def.Ptr()))
						{
                            cresult.GetErrorWriter()->diagnose(def->SyntaxNode->Position, Diagnostics::componentDefinitionCircularity, def->OriginalName);
							return nullptr;
						}
					}
					/*
					// eliminate redundant (downstream) definitions, one at a time
					auto comps = result->GetComponentDependencyOrder();
					for (int i = comps.Count() - 1; i >= 0; i--)
					{
						auto comp = comps[i];
						auto & defs = result->DefinitionsByComponent[comp->UniqueName]();
						EnumerableHashSet<ComponentDefinitionIR*> removedDefs;
						for (auto & def : defs)
							if (!def.Value->IsEntryPoint && !comp->Type->PinnedWorlds.Contains(def.Value->World))
							{
								for (auto & otherDef : defs)
								{
									if (otherDef.Value != def.Value && !removedDefs.Contains(otherDef.Value)
										&& shader->Pipeline->IsWorldReachable(otherDef.Value->World, def.Value->World))
									{
										removedDefs.Add(def.Value);
										break;
									}
								}
							}
						if (removedDefs.Count())
						{
							result->RemoveDefinitions([&](ComponentDefinitionIR* def) {return removedDefs.Contains(def); });
							changed = true;
						}
					}
					*/
				}
				return result;
			}
		public:
			virtual CompileUnit Parse(CompileResult & result, String source, String fileName, IncludeHandler* includeHandler, Dictionary<String,String> const& preprocesorDefinitions) override
			{
                auto tokens = PreprocessSource(source, fileName, result.GetErrorWriter(), includeHandler, preprocesorDefinitions);
				CompileUnit rs;
                rs.SyntaxNode = ParseProgram(tokens, result.GetErrorWriter(), fileName);
				return rs;
			}
			virtual void Compile(CompileResult & result, CompilationContext & context, List<CompileUnit> & units, const CompileOptions & options) override
			{
				RefPtr<ProgramSyntaxNode> programSyntaxNode = new ProgramSyntaxNode();
				for (auto & unit : units)
				{
					programSyntaxNode->Include(unit.SyntaxNode.Ptr());
				}

				SymbolTable & symTable = context.Symbols;
				auto & shaderClosures = context.ShaderClosures;
				RefPtr<SyntaxVisitor> visitor = CreateSemanticsVisitor(&symTable, result.GetErrorWriter());
				try
				{
					programSyntaxNode->Accept(visitor.Ptr());
					visitor = nullptr;
					if (result.GetErrorCount() > 0)
						return;
					symTable.EvalFunctionReferenceClosure();
					if (result.GetErrorCount() > 0)
						return;

					for (auto & shader : symTable.ShaderDependenceOrder)
					{
						if (shader->IsAbstract)
							continue;
						if (!shaderClosures.ContainsKey(shader->SyntaxNode->Name.Content))
						{
							auto shaderClosure = CreateShaderClosure(result.GetErrorWriter(), &symTable, shader);
							FlattenShaderClosure(result.GetErrorWriter(), &symTable, shaderClosure.Ptr());
							shaderClosures.Add(shader->SyntaxNode->Name.Content, shaderClosure);
						}
					}
					
					ResolveAttributes(&symTable);

					if (result.GetErrorCount() > 0)
						return;
					CodeGenBackend * backend = nullptr;
					switch(options.Target)
					{
					case CodeGenTarget::SPIRV:
						backend = backends["spirv"]().Ptr();
						break;
					case CodeGenTarget::GLSL:
						backend = backends["gls"]().Ptr();
						break;
					case CodeGenTarget::HLSL:
						backend = backends["hls"]().Ptr();
						break;
					default:
						// TODO: emit an appropriate diagnostic
						return;
					}

					Schedule schedule;
					if (options.ScheduleSource != "")
					{
						schedule = Schedule::Parse(options.ScheduleSource, options.ScheduleFileName, result.GetErrorWriter());
					}
					for (auto shader : shaderClosures)
					{
						// generate shader variant from schedule file, and also apply mechanic deduction rules
						if (!shader.Value->IR)
							shader.Value->IR = GenerateShaderVariantIR(result, shader.Value.Ptr(), schedule, &symTable);
					}
					if (options.Mode == CompilerMode::ProduceShader)
					{
						if (result.GetErrorWriter()->GetErrorCount() > 0)
							return;
						// generate IL code
						
						RefPtr<ICodeGenerator> codeGen = CreateCodeGenerator(&symTable, result);
						if (context.Program)
						{
							result.Program->Functions = context.Program->Functions;
							result.Program->Shaders = context.Program->Shaders;
							result.Program->Structs = context.Program->Structs;
							result.Program->ConstantPool = context.Program->ConstantPool;
						}
						for (auto & s : programSyntaxNode->Structs)
							codeGen->ProcessStruct(s.Ptr());

						for (auto & func : programSyntaxNode->Functions)
							codeGen->ProcessFunction(func.Ptr());
						for (auto & shader : shaderClosures)
						{
							InsertImplicitImportOperators(result.GetErrorWriter(), shader.Value->IR.Ptr());
						}
						if (result.GetErrorCount() > 0)
							return;
						for (auto & shader : shaderClosures)
						{
							codeGen->ProcessShader(shader.Value->IR.Ptr());
						}
						if (result.GetErrorCount() > 0)
							return;
						// emit target code
						EnumerableHashSet<String> symbolsToGen;
						for (auto & unit : units)
						{
							for (auto & shader : unit.SyntaxNode->Shaders)
								if (!shader->IsModule)
									symbolsToGen.Add(shader->Name.Content);
							for (auto & func : unit.SyntaxNode->Functions)
								symbolsToGen.Add(func->Name);
						}
						auto IsSymbolToGen = [&](String & shaderName)
						{
							if (symbolsToGen.Contains(shaderName))
								return true;
							for (auto & symbol : symbolsToGen)
								if (shaderName.StartsWith(symbol))
									return true;
							return false;
						};
						for (auto & shader : result.Program->Shaders)
						{
							if ((options.SymbolToCompile.Length() == 0 && IsSymbolToGen(shader->Name))
								|| options.SymbolToCompile == shader->Name)
							{
								StringBuilder glslBuilder;
								Dictionary<String, String> targetCode;
								result.CompiledSource[shader->Name] = backend->GenerateShader(result, &symTable, shader.Ptr(), result.GetErrorWriter());
							}
						}
					}
					else if (options.Mode == CompilerMode::GenerateChoice)
					{
						for (auto shader : shaderClosures)
						{
							if (options.SymbolToCompile.Length() == 0 || shader.Value->Name == options.SymbolToCompile)
							{
								auto &worldOrder = shader.Value->Pipeline->GetWorldTopologyOrder();
								for (auto & comp : shader.Value->AllComponents)
								{
									ShaderChoice choice;
									if (comp.Value->ChoiceNames.Count() == 0)
										continue;
									if (comp.Value->IsParam())
										continue;
									choice.ChoiceName = comp.Value->ChoiceNames.First();
									for (auto & impl : comp.Value->Implementations)
									{
										for (auto w : impl->Worlds)
											if (comp.Value->Type->ConstrainedWorlds.Contains(w))
												choice.Options.Add(ShaderChoiceValue(w, impl->AlternateName));
									}
									if (auto defs = shader.Value->IR->DefinitionsByComponent.TryGetValue(comp.Key))
									{
										int latestWorldOrder = -1;
										for (auto & def : *defs)
										{
											int order = worldOrder.IndexOf(def.Key);
											if (latestWorldOrder < order)
											{
												choice.DefaultValue = def.Key;
												latestWorldOrder = order;
											}
										}
									}
									result.Choices.Add(choice);
								}
							}
						}
					}
					else
					{
                        result.GetErrorWriter()->diagnose(CodePosition(), Diagnostics::unsupportedCompilerMode);
						return;
					}
					context.Program = result.Program;
				}
				catch (int)
				{
				}
				catch (...)
				{
					throw;
				}
				return;
			}

			ShaderCompilerImpl()
			{
				if (compilerInstances == 0)
				{
					BasicExpressionType::Init();
				}
				compilerInstances++;
				backends.Add("gls", CreateGLSLCodeGen());
				backends.Add("hls", CreateHLSLCodeGen());
				backends.Add("spirv", CreateSpirVCodeGen());
			}

			~ShaderCompilerImpl()
			{
				compilerInstances--;
				if (compilerInstances == 0)
				{
					BasicExpressionType::Finalize();
					SpireStdLib::Finalize();
				}
			}
		};

		ShaderCompiler * CreateShaderCompiler()
		{
			return new ShaderCompilerImpl();
		}

	}
}