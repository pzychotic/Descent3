set(HEADERS tanarchystr.h tanDMFC.h)
set(CPPS tanarchy.cpp)

set(NETGAME_MODULE tanarchy)

add_library(${NETGAME_MODULE} MODULE ${CPPS} ${HEADERS})
set_target_properties(${NETGAME_MODULE} PROPERTIES CXX_VISIBILITY_PRESET "hidden")
set_target_properties(${NETGAME_MODULE} PROPERTIES PREFIX "")
set_target_properties(${NETGAME_MODULE} PROPERTIES SUFFIX ".d3m")
set_target_properties(${NETGAME_MODULE} PROPERTIES LIBRARY_OUTPUT_NAME "team anarchy")
set_target_properties(${NETGAME_MODULE} PROPERTIES LIBRARY_OUTPUT_DIRECTORY "$<TARGET_FILE_DIR:Descent3>/netgames")

target_link_libraries(${NETGAME_MODULE} PRIVATE
  dmfc
)
install(TARGETS "${NETGAME_MODULE}" DESTINATION ${CMAKE_INSTALL_LIBDIR}/netgames)
